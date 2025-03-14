//---------------------------------------------------------------------------
//
//	SCSI Target Emulator RaSCSI (*^..^*)
//	for Raspberry Pi
//
//	Powered by XM6 TypeG Technology.
//	Copyright (C) 2016-2020 GIMONS
//	Copyright (C) 2020-2021 Contributors to the RaSCSI project
//	[ RaSCSI main ]
//
//---------------------------------------------------------------------------

#include "os.h"
#include "controllers/sasidev_ctrl.h"
#include "devices/device_factory.h"
#include "devices/device.h"
#include "devices/disk.h"
#include "devices/file_support.h"
#include "gpiobus.h"
#include "exceptions.h"
#include "protobuf_util.h"
#include "rascsi_version.h"
#include "rascsi_response.h"
#include "rasutil.h"
#include "rascsi_image.h"
#include "rascsi_interface.pb.h"
#include "spdlog/spdlog.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include <string>
#include <sstream>
#include <iostream>
#include <fstream>
#include <list>
#include <vector>
#include <map>
#include "config.h"

using namespace std;
using namespace spdlog;
using namespace rascsi_interface;
using namespace ras_util;
using namespace protobuf_util;

//---------------------------------------------------------------------------
//
//  Constant declarations
//
//---------------------------------------------------------------------------
#define CtrlMax	8					// Maximum number of SCSI controllers
#define UnitNum	SASIDEV::UnitMax	// Number of units around controller
#define FPRT(fp, ...) fprintf(fp, __VA_ARGS__ )

#define COMPONENT_SEPARATOR ':'

//---------------------------------------------------------------------------
//
//	Variable declarations
//
//---------------------------------------------------------------------------
static volatile bool running;		// Running flag
static volatile bool active;		// Processing flag
vector<SASIDEV *> controllers(CtrlMax);	// Controllers
vector<Device *> devices(CtrlMax * UnitNum);	// Disks
GPIOBUS *bus;						// GPIO Bus
int monsocket;						// Monitor Socket
pthread_t monthread;				// Monitor Thread
pthread_mutex_t ctrl_mutex;					// Semaphore for the ctrl array
static void *MonThread(void *param);
string current_log_level;			// Some versions of spdlog do not support get_log_level()
string access_token;
unordered_set<int> reserved_ids;
DeviceFactory& device_factory = DeviceFactory::instance();
RascsiImage rascsi_image;
RascsiResponse rascsi_response(&device_factory, &rascsi_image);

//---------------------------------------------------------------------------
//
//	Signal Processing
//
//---------------------------------------------------------------------------
void KillHandler(int sig)
{
	// Stop instruction
	running = false;
}

//---------------------------------------------------------------------------
//
//	Banner Output
//
//---------------------------------------------------------------------------
void Banner(int argc, char* argv[])
{
	FPRT(stdout,"SCSI Target Emulator RaSCSI Reloaded ");
	FPRT(stdout,"version %s (%s, %s)\n",
		rascsi_get_version_string(),
		__DATE__,
		__TIME__);
	FPRT(stdout,"Powered by XM6 TypeG Technology / ");
	FPRT(stdout,"Copyright (C) 2016-2020 GIMONS\n");
	FPRT(stdout,"Copyright (C) 2020-2022 Contributors to the RaSCSI Reloaded project\n");
	FPRT(stdout,"Connect type : %s\n", CONNECT_DESC);

	if ((argc > 1 && strcmp(argv[1], "-h") == 0) ||
		(argc > 1 && strcmp(argv[1], "--help") == 0)){
		FPRT(stdout,"\n");
		FPRT(stdout,"Usage: %s [-IDn FILE] ...\n\n", argv[0]);
		FPRT(stdout," n is SCSI identification number(0-7).\n");
		FPRT(stdout," FILE is disk image file.\n\n");
		FPRT(stdout,"Usage: %s [-HDn FILE] ...\n\n", argv[0]);
		FPRT(stdout," n is X68000 SASI HD number(0-15).\n");
		FPRT(stdout," FILE is disk image file, \"daynaport\", \"bridge\", \"printer\" or \"services\".\n\n");
		FPRT(stdout," Image type is detected based on file extension.\n");
		FPRT(stdout,"  hdf : SASI HD image (XM6 SASI HD image)\n");
		FPRT(stdout,"  hds : SCSI HD image (Non-removable generic SCSI HD image)\n");
		FPRT(stdout,"  hdr : SCSI HD image (Removable generic SCSI HD image)\n");
		FPRT(stdout,"  hdn : SCSI HD image (NEC GENUINE)\n");
		FPRT(stdout,"  hdi : SCSI HD image (Anex86 HD image)\n");
		FPRT(stdout,"  nhd : SCSI HD image (T98Next HD image)\n");
		FPRT(stdout,"  mos : SCSI MO image (MO image)\n");
		FPRT(stdout,"  iso : SCSI CD image (ISO 9660 image)\n");

		exit(EXIT_SUCCESS);
	}
}

//---------------------------------------------------------------------------
//
//	Initialization
//
//---------------------------------------------------------------------------

bool InitService(int port)
{
	int result = pthread_mutex_init(&ctrl_mutex,NULL);
	if (result != EXIT_SUCCESS){
		LOGERROR("Unable to create a mutex. Error code: %d", result);
		return false;
	}

	// Create socket for monitor
	struct sockaddr_in server;
	monsocket = socket(PF_INET, SOCK_STREAM, 0);
	memset(&server, 0, sizeof(server));
	server.sin_family = PF_INET;
	server.sin_port   = htons(port);
	server.sin_addr.s_addr = htonl(INADDR_ANY);

	// allow address reuse
	int yes = 1;
	if (setsockopt(monsocket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
		return false;
	}

	signal(SIGPIPE, SIG_IGN);

	// Bind
	if (bind(monsocket, (struct sockaddr *)&server,
		sizeof(struct sockaddr_in)) < 0) {
		FPRT(stderr, "Error: Already running?\n");
		return false;
	}

	// Create Monitor Thread
	pthread_create(&monthread, NULL, MonThread, NULL);

	// Interrupt handler settings
	if (signal(SIGINT, KillHandler) == SIG_ERR) {
		return false;
	}
	if (signal(SIGHUP, KillHandler) == SIG_ERR) {
		return false;
	}
	if (signal(SIGTERM, KillHandler) == SIG_ERR) {
		return false;
	}

	running = false;
	active = false;

	return true;
}

bool InitBus()
{
	// GPIOBUS creation
	bus = new GPIOBUS();

	// GPIO Initialization
	if (!bus->Init()) {
		return false;
	}

	// Bus Reset
	bus->Reset();

	return true;
}

//---------------------------------------------------------------------------
//
//	Cleanup
//
//---------------------------------------------------------------------------
void Cleanup()
{
	// Delete the disks
	for (auto it = devices.begin(); it != devices.end(); ++it) {
		if (*it) {
			delete *it;
			*it = NULL;
		}
	}

	// Delete the Controllers
	for (auto it = controllers.begin(); it != controllers.end(); ++it) {
		if (*it) {
			delete *it;
			*it = NULL;
		}
	}

	// Cleanup the Bus
	if (bus) {
		bus->Cleanup();

		// Discard the GPIOBUS object
		delete bus;
	}

	// Close the monitor socket
	if (monsocket >= 0) {
		close(monsocket);
	}

	pthread_mutex_destroy(&ctrl_mutex);
}

//---------------------------------------------------------------------------
//
//	Reset
//
//---------------------------------------------------------------------------
void Reset()
{
	// Reset all of the controllers
	for (const auto& controller : controllers) {
		if (controller) {
			controller->Reset();
		}
	}

	// Reset the bus
	bus->Reset();
}

//---------------------------------------------------------------------------
//
//	Controller Mapping
//
//---------------------------------------------------------------------------
bool MapController(Device **map)
{
	assert(bus);

	bool status = true;

	// Take ownership of the ctrl data structure
	pthread_mutex_lock(&ctrl_mutex);

	// Replace the changed unit
	for (size_t i = 0; i < controllers.size(); i++) {
		for (int j = 0; j < UnitNum; j++) {
			int unitno = i * UnitNum + j;
			if (devices[unitno] != map[unitno]) {
				// Check if the original unit exists
				if (devices[unitno]) {
					// Disconnect it from the controller
					if (controllers[i]) {
						controllers[i]->SetUnit(j, NULL);
					}

					// Free the Unit
					delete devices[unitno];
				}

				// Setup a new unit
				devices[unitno] = map[unitno];
			}
		}
	}

	// Reconfigure all of the controllers
	int i = 0;
	for (auto it = controllers.begin(); it != controllers.end(); ++i, ++it) {
		// Examine the unit configuration
		int sasi_num = 0;
		int scsi_num = 0;
		for (int j = 0; j < UnitNum; j++) {
			int unitno = i * UnitNum + j;
			// branch by unit type
			if (devices[unitno]) {
				if (devices[unitno]->IsSASIHD()) {
					// Drive is SASI, so increment SASI count
					sasi_num++;
				} else {
					// Drive is SCSI, so increment SCSI count
					scsi_num++;
				}
			}

			// Remove the unit
			if (*it) {
				(*it)->SetUnit(j, NULL);
			}
		}

		// If there are no units connected
		if (!sasi_num && !scsi_num) {
			if (*it) {
				delete *it;
				*it = NULL;
				continue;
			}
		}

		// Mixture of SCSI and SASI
		if (sasi_num > 0 && scsi_num > 0) {
			status = false;
			continue;
		}

		if (sasi_num > 0) {
			// Only SASI Unit(s)

			// Release the controller if it is not SASI
			if (*it && !(*it)->IsSASI()) {
				delete *it;
				*it = NULL;
			}

			// Create a new SASI controller
			if (!*it) {
				*it = new SASIDEV();
				(*it)->Connect(i, bus);
			}
		} else {
			// Only SCSI Unit(s)

			// Release the controller if it is not SCSI
			if (*it && !(*it)->IsSCSI()) {
				delete *it;
				*it = NULL;
			}

			// Create a new SCSI controller
			if (!*it) {
				*it = new SCSIDEV();
				(*it)->Connect(i, bus);
			}
		}

		// connect all units
		for (int j = 0; j < UnitNum; j++) {
			int unitno = i * UnitNum + j;
			if (devices[unitno]) {
				// Add the unit connection
				(*it)->SetUnit(j, (static_cast<Disk *>(devices[unitno])));
			}
		}
	}
	pthread_mutex_unlock(&ctrl_mutex);

	return status;
}

bool ReadAccessToken(const char *filename)
{
	struct stat st;
	if (stat(filename, &st) || !S_ISREG(st.st_mode)) {
		cerr << "Can't access token file '" << optarg << "'" << endl;
		return false;
	}

	if (st.st_uid || st.st_gid || (st.st_mode & (S_IROTH | S_IWOTH | S_IRGRP | S_IWGRP))) {
		cerr << "Access token file '" << optarg << "' must be owned by root and readable by root only" << endl;
		return false;
	}

	ifstream token_file(filename, ifstream::in);
	if (token_file.fail()) {
		cerr << "Can't open access token file '" << optarg << "'" << endl;
		return false;
	}

	getline(token_file, access_token);
	if (token_file.fail()) {
		token_file.close();
		cerr << "Can't read access token file '" << optarg << "'" << endl;
		return false;
	}

	if (access_token.empty()) {
		token_file.close();
		cerr << "Access token file '" << optarg << "' must not be empty" << endl;
		return false;
	}

	token_file.close();

	return true;
}

string ValidateLunSetup(const PbCommand& command, const vector<Device *>& existing_devices)
{
	// Mapping of available LUNs (bit vector) to devices
	map<uint32_t, uint32_t> luns;

	// Collect LUN vectors of new devices
	for (const auto& device : command.devices()) {
		luns[device.id()] |= 1 << device.unit();
	}

	// Collect LUN vectors of existing devices
	for (auto const& device : existing_devices) {
		if (device) {
			luns[device->GetId()] |= 1 << device->GetLun();
		}
	}

	// LUN 0 must exist for all devices
	for (auto const& [id, lun]: luns) {
		if (!(lun & 0x01)) {
			return "LUN 0 is missing for device ID " + to_string(id);
		}
	}

	return "";
}

bool SetLogLevel(const string& log_level)
{
	if (log_level == "trace") {
		set_level(level::trace);
	}
	else if (log_level == "debug") {
		set_level(level::debug);
	}
	else if (log_level == "info") {
		set_level(level::info);
	}
	else if (log_level == "warn") {
		set_level(level::warn);
	}
	else if (log_level == "err") {
		set_level(level::err);
	}
	else if (log_level == "critical") {
		set_level(level::critical);
	}
	else if (log_level == "off") {
		set_level(level::off);
	}
	else {
		return false;
	}

	current_log_level = log_level;

	LOGINFO("Set log level to '%s'", current_log_level.c_str());

	return true;
}

void LogDevices(const string& devices)
{
	stringstream ss(devices);
	string line;

	while (getline(ss, line, '\n')) {
		LOGINFO("%s", line.c_str());
	}
}

string SetReservedIds(const string& ids)
{
	list<string> ids_to_reserve;
	stringstream ss(ids);
    string id;
    while (getline(ss, id, ',')) {
    	if (!id.empty()) {
    		ids_to_reserve.push_back(id);
    	}
    }

    unordered_set<int> reserved;
    for (string id_to_reserve : ids_to_reserve) {
    	int id;
 		if (!GetAsInt(id_to_reserve, id) || id > 7) {
 			return "Invalid ID " + id_to_reserve;
 		}

 		if (devices[id * UnitNum]) {
 			return "ID " + id_to_reserve + " is currently in use";
 		}

 		reserved.insert(id);
    }

    reserved_ids = reserved;

    if (!reserved_ids.empty()) {
    	string s;
    	bool isFirst = true;
    	for (auto const& reserved_id : reserved_ids) {
    		if (!isFirst) {
    			s += ", ";
    		}
    		isFirst = false;
    		s += to_string(reserved_id);
    	}

    	LOGINFO("Reserved ID(s) set to %s", s.c_str());
    }
    else {
    	LOGINFO("Cleared reserved IDs");
    }

	return "";
}

void DetachAll()
{
	Device *map[devices.size()];
	for (size_t i = 0; i < devices.size(); i++) {
		map[i] = NULL;
	}

	if (MapController(map)) {
		LOGINFO("Detached all devices");
	}

	FileSupport::UnreserveAll();
}

bool Attach(const CommandContext& context, const PbDeviceDefinition& pb_device, Device *map[], bool dryRun)
{
	const int id = pb_device.id();
	const int unit = pb_device.unit();
	const PbDeviceType type = pb_device.type();

	if (map[id * UnitNum + unit]) {
		return ReturnLocalizedError(context, ERROR_DUPLICATE_ID, to_string(id), to_string(unit));
	}

	string filename = GetParam(pb_device, "file");

	// Create a new device, based on the provided type or filename
	Device *device = device_factory.CreateDevice(type, filename);
	if (!device) {
		if (type == UNDEFINED) {
			return ReturnLocalizedError(context, ERROR_MISSING_DEVICE_TYPE, filename);
		}
		else {
			return ReturnLocalizedError(context, ERROR_UNKNOWN_DEVICE_TYPE, PbDeviceType_Name(type));
		}
	}

	int supported_luns = device->GetSupportedLuns();
	if (unit >= supported_luns) {
		delete device;

		string error = "Invalid unit " + to_string(unit) + " for device type " + PbDeviceType_Name(type);
		if (supported_luns == 1) {
			error += " (0)";
		}
		else {
			error += " (0-" + to_string(supported_luns -1) + ")";
		}
		return ReturnStatus(context, false, error);
	}

	// If no filename was provided the medium is considered removed
	FileSupport *file_support = dynamic_cast<FileSupport *>(device);
	if (file_support) {
		device->SetRemoved(filename.empty());
	}
	else {
		device->SetRemoved(false);
	}

	device->SetId(id);
	device->SetLun(unit);

	try {
		if (!pb_device.vendor().empty()) {
			device->SetVendor(pb_device.vendor());
		}
		if (!pb_device.product().empty()) {
			device->SetProduct(pb_device.product());
		}
		if (!pb_device.revision().empty()) {
			device->SetRevision(pb_device.revision());
		}
	}
	catch(const illegal_argument_exception& e) {
		return ReturnStatus(context, false, e.getmsg());
	}

	if (pb_device.block_size()) {
		Disk *disk = dynamic_cast<Disk *>(device);
		if (disk && disk->IsSectorSizeConfigurable()) {
			if (!disk->SetConfiguredSectorSize(pb_device.block_size())) {
				delete device;

				return ReturnLocalizedError(context, ERROR_BLOCK_SIZE, to_string(pb_device.block_size()));
			}
		}
		else {
			delete device;

			return ReturnLocalizedError(context, ERROR_BLOCK_SIZE_NOT_CONFIGURABLE, PbDeviceType_Name(type));
		}
	}

	// File check (type is HD, for removable media drives, CD and MO the medium (=file) may be inserted later
	if (file_support && !device->IsRemovable() && filename.empty()) {
		delete device;

		return ReturnStatus(context, false, "Device type " + PbDeviceType_Name(type) + " requires a filename");
	}

	Filepath filepath;
	if (file_support && !filename.empty()) {
		filepath.SetPath(filename.c_str());
		string initial_filename = filepath.GetPath();

		int id;
		int unit;
		if (FileSupport::GetIdsForReservedFile(filepath, id, unit)) {
			delete device;

			return ReturnLocalizedError(context, ERROR_IMAGE_IN_USE, filename, to_string(id), to_string(unit));
		}

		try {
			try {
				file_support->Open(filepath);
			}
			catch(const file_not_found_exception&) {
				// If the file does not exist search for it in the default image folder
				filepath.SetPath(string(rascsi_image.GetDefaultImageFolder() + "/" + filename).c_str());

				if (FileSupport::GetIdsForReservedFile(filepath, id, unit)) {
					delete device;

					return ReturnLocalizedError(context, ERROR_IMAGE_IN_USE, filename, to_string(id), to_string(unit));
				}

				file_support->Open(filepath);
			}
		}
		catch(const io_exception& e) {
			delete device;

			return ReturnLocalizedError(context, ERROR_FILE_OPEN, initial_filename, e.getmsg());
		}

		file_support->ReserveFile(filepath, device->GetId(), device->GetLun());
	}

	// Only non read-only devices support protect/unprotect
	// This operation must not be executed before Open() because Open() overrides some settings.
	if (device->IsProtectable() && !device->IsReadOnly()) {
		device->SetProtected(pb_device.protected_());
	}

	// Stop the dry run here, before permanently modifying something
	if (dryRun) {
		delete device;

		return true;
	}

	unordered_map<string, string> params = { pb_device.params().begin(), pb_device.params().end() };
	if (!device->SupportsFile()) {
		params.erase("file");
	}
	if (!device->Init(params)) {
		delete device;

		return ReturnStatus(context, false, "Initialization of " + PbDeviceType_Name(type) + " device, ID " +to_string(id) +
				", unit " +to_string(unit) + " failed");
	}

	// Replace with the newly created unit
	map[id * UnitNum + unit] = device;

	// Re-map the controller
	if (MapController(map)) {
		string msg = "Attached ";
		if (device->IsReadOnly()) {
			msg += "read-only ";
		}
		else if (device->IsProtectable() && device->IsProtected()) {
			msg += "protected ";
		}
		msg += device->GetType() + " device, ID " + to_string(id) + ", unit " + to_string(unit);
		LOGINFO("%s", msg.c_str());

		return true;
	}

	return ReturnLocalizedError(context, ERROR_SASI_SCSI);
}

bool Detach(const CommandContext& context, Device *device, Device *map[], bool dryRun)
{
	if (!device->GetLun()) {
		for (auto const& d : devices) {
			// LUN 0 can only be detached if there is no other lUN anymore
			if (d && d->GetId() == device->GetId() && d->GetLun()) {
				return ReturnStatus(context, false, "LUN 0 cannot be detached as long as there is still another LUN");
			}
		}
	}

	if (!dryRun) {
		map[device->GetId() * UnitNum + device->GetLun()] = NULL;

		FileSupport *file_support = dynamic_cast<FileSupport *>(device);
		if (file_support) {
			file_support->UnreserveFile();
		}

		LOGINFO("Detached %s device with ID %d, unit %d", device->GetType().c_str(), device->GetId(), device->GetLun());

		// Re-map the controller
		MapController(map);
	}

	return true;
}

bool Insert(const CommandContext& context, const PbDeviceDefinition& pb_device, Device *device, bool dryRun)
{
	if (!device->IsRemoved()) {
		return ReturnLocalizedError(context, ERROR_EJECT_REQUIRED);
	}

	if (!pb_device.vendor().empty() || !pb_device.product().empty() || !pb_device.revision().empty()) {
		return ReturnLocalizedError(context, ERROR_DEVICE_NAME_UPDATE);
	}

	string filename = GetParam(pb_device, "file");
	if (filename.empty()) {
		return ReturnLocalizedError(context, ERROR_MISSING_FILENAME);
	}

	if (dryRun) {
		return true;
	}

	LOGINFO("Insert %sfile '%s' requested into %s ID %d, unit %d", pb_device.protected_() ? "protected " : "",
			filename.c_str(), device->GetType().c_str(), pb_device.id(), pb_device.unit());

	Disk *disk = dynamic_cast<Disk *>(device);

	if (pb_device.block_size()) {
		if (disk && disk->IsSectorSizeConfigurable()) {
			if (!disk->SetConfiguredSectorSize(pb_device.block_size())) {
				return ReturnLocalizedError(context, ERROR_BLOCK_SIZE, to_string(pb_device.block_size()));
			}
		}
		else {
			return ReturnLocalizedError(context, ERROR_BLOCK_SIZE_NOT_CONFIGURABLE, device->GetType());
		}
	}

	int id;
	int unit;
	Filepath filepath;
	filepath.SetPath(filename.c_str());
	string initial_filename = filepath.GetPath();

	if (FileSupport::GetIdsForReservedFile(filepath, id, unit)) {
		return ReturnLocalizedError(context, ERROR_IMAGE_IN_USE, filename, to_string(id), to_string(unit));
	}

	FileSupport *file_support = dynamic_cast<FileSupport *>(device);
	try {
		try {
			file_support->Open(filepath);
		}
		catch(const file_not_found_exception&) {
			// If the file does not exist search for it in the default image folder
			filepath.SetPath((rascsi_image.GetDefaultImageFolder() + "/" + filename).c_str());

			if (FileSupport::GetIdsForReservedFile(filepath, id, unit)) {
				return ReturnLocalizedError(context, ERROR_IMAGE_IN_USE, filename, to_string(id), to_string(unit));
			}

			file_support->Open(filepath);
		}
	}
	catch(const io_exception& e) {
		return ReturnLocalizedError(context, ERROR_FILE_OPEN, initial_filename, e.getmsg());
	}

	file_support->ReserveFile(filepath, device->GetId(), device->GetLun());

	// Only non read-only devices support protect/unprotect.
	// This operation must not be executed before Open() because Open() overrides some settings.
	if (device->IsProtectable() && !device->IsReadOnly()) {
		device->SetProtected(pb_device.protected_());
	}

	if (disk) {
		disk->MediumChanged();
	}

	return true;
}

void TerminationHandler(int signum)
{
	DetachAll();

	Cleanup();

	exit(signum);
}

//---------------------------------------------------------------------------
//
//	Command Processing
//
//---------------------------------------------------------------------------

bool ProcessCmd(const CommandContext& context, const PbDeviceDefinition& pb_device, const PbCommand& command, bool dryRun)
{
	const int id = pb_device.id();
	const int unit = pb_device.unit();
	const PbDeviceType type = pb_device.type();
	const PbOperation operation = command.operation();
	const map<string, string> params = { command.params().begin(), command.params().end() };

	ostringstream s;
	s << (dryRun ? "Validating: " : "Executing: ");
	s << "operation=" << PbOperation_Name(operation);

	if (!params.empty()) {
		s << ", command params=";
		bool isFirst = true;
		for (const auto& param: params) {
			if (!isFirst) {
				s << ", ";
			}
			isFirst = false;
			string value = param.first != "token" ? param.second : "???";
			s << "'" << param.first << "=" << value << "'";
		}
	}

	s << ", device id=" << id << ", unit=" << unit << ", type=" << PbDeviceType_Name(type);

	if (pb_device.params_size()) {
		s << ", device params=";
		bool isFirst = true;
		for (const auto& param: pb_device.params()) {
			if (!isFirst) {
				s << ":";
			}
			isFirst = false;
			s << "'" << param.first << "=" << param.second << "'";
		}
	}

	s << ", vendor='" << pb_device.vendor() << "', product='" << pb_device.product()
			<< "', revision='" << pb_device.revision()
			<< "', block size=" << pb_device.block_size();
	LOGINFO("%s", s.str().c_str());

	// Check the Controller Number
	if (id < 0) {
		return ReturnLocalizedError(context, ERROR_MISSING_DEVICE_ID);
	}
	if (id >= CtrlMax) {
		return ReturnStatus(context, false, "Invalid device ID " + to_string(id) + " (0-" + to_string(CtrlMax - 1) + ")");
	}

	if (operation == ATTACH && reserved_ids.find(id) != reserved_ids.end()) {
		return ReturnLocalizedError(context, ERROR_RESERVED_ID, to_string(id));
	}

	// Check the Unit Number
	if (unit < 0 || unit >= UnitNum) {
		return ReturnStatus(context, false, "Invalid unit " + to_string(unit) + " (0-" + to_string(UnitNum - 1) + ")");
	}

	// Copy the devices
	Device *map[devices.size()];
	for (size_t i = 0; i < devices.size(); i++) {
		map[i] = devices[i];
	}

	if (operation == ATTACH) {
		return Attach(context, pb_device, map, dryRun);
	}

	// Does the controller exist?
	if (!dryRun && !controllers[id]) {
		return ReturnLocalizedError(context, ERROR_NON_EXISTING_DEVICE, to_string(id));
	}

	// Does the unit exist?
	Device *device = devices[id * UnitNum + unit];
	if (!device) {
		return ReturnLocalizedError(context, ERROR_NON_EXISTING_UNIT, to_string(id), to_string(unit));
	}

	if (operation == DETACH) {
		return Detach(context, device, map, dryRun);
	}

	if ((operation == START || operation == STOP) && !device->IsStoppable()) {
		return ReturnStatus(context, false, PbOperation_Name(operation) + " operation denied (" + device->GetType() + " isn't stoppable)");
	}

	if ((operation == INSERT || operation == EJECT) && !device->IsRemovable()) {
		return ReturnStatus(context, false, PbOperation_Name(operation) + " operation denied (" + device->GetType() + " isn't removable)");
	}

	if ((operation == PROTECT || operation == UNPROTECT) && !device->IsProtectable()) {
		return ReturnStatus(context, false, PbOperation_Name(operation) + " operation denied (" + device->GetType() + " isn't protectable)");
	}
	if ((operation == PROTECT || operation == UNPROTECT) && !device->IsReady()) {
		return ReturnStatus(context, false, PbOperation_Name(operation) + " operation denied (" + device->GetType() + " isn't ready)");
	}

	switch (operation) {
		case START:
			if (!dryRun) {
				LOGINFO("Start requested for %s ID %d, unit %d", device->GetType().c_str(), id, unit);

				if (!device->Start()) {
					LOGWARN("Starting %s ID %d, unit %d failed", device->GetType().c_str(), id, unit);
				}
			}
			break;

		case STOP:
			if (!dryRun) {
				LOGINFO("Stop requested for %s ID %d, unit %d", device->GetType().c_str(), id, unit);

				// STOP is idempotent
				device->Stop();
			}
			break;

		case INSERT:
			return Insert(context, pb_device, device, dryRun);

		case EJECT:
			if (!dryRun) {
				LOGINFO("Eject requested for %s ID %d, unit %d", device->GetType().c_str(), id, unit);

				if (!device->Eject(true)) {
					LOGWARN("Ejecting %s ID %d, unit %d failed", device->GetType().c_str(), id, unit);
				}
			}
			break;

		case PROTECT:
			if (!dryRun) {
				LOGINFO("Write protection requested for %s ID %d, unit %d", device->GetType().c_str(), id, unit);

				// PROTECT is idempotent
				device->SetProtected(true);
			}
			break;

		case UNPROTECT:
			if (!dryRun) {
				LOGINFO("Write unprotection requested for %s ID %d, unit %d", device->GetType().c_str(), id, unit);

				// UNPROTECT is idempotent
				device->SetProtected(false);
			}
			break;

		case ATTACH:
		case DETACH:
			// The non dry-run case has been handled before the switch
			assert(dryRun);
			break;

		case CHECK_AUTHENTICATION:
		case NO_OPERATION:
			// Do nothing, just log
			LOGTRACE("Received %s command", PbOperation_Name(operation).c_str());
			break;

		default:
			return ReturnLocalizedError(context, ERROR_OPERATION);
	}

	return true;
}

bool ProcessCmd(const CommandContext& context, const PbCommand& command)
{
	switch (command.operation()) {
		case DETACH_ALL:
			DetachAll();
			return ReturnStatus(context);

		case RESERVE_IDS: {
			const string ids = GetParam(command, "ids");
			string error = SetReservedIds(ids);
			if (!error.empty()) {
				return ReturnStatus(context, false, error);
			}

			return ReturnStatus(context);
		}

		case CREATE_IMAGE:
			return rascsi_image.CreateImage(context, command);

		case DELETE_IMAGE:
			return rascsi_image.DeleteImage(context, command);

		case RENAME_IMAGE:
			return rascsi_image.RenameImage(context, command);

		case COPY_IMAGE:
			return rascsi_image.CopyImage(context, command);

		case PROTECT_IMAGE:
		case UNPROTECT_IMAGE:
			return rascsi_image.SetImagePermissions(context, command);

		default:
			// This is a device-specific command handled below
			break;
	}

	// Remember the list of reserved files, than run the dry run
	const auto reserved_files = FileSupport::GetReservedFiles();
	for (const auto& device : command.devices()) {
		if (!ProcessCmd(context, device, command, true)) {
			// Dry run failed, restore the file list
			FileSupport::SetReservedFiles(reserved_files);
			return false;
		}
	}

	// Restore the list of reserved files before proceeding
	FileSupport::SetReservedFiles(reserved_files);

	string result = ValidateLunSetup(command, devices);
	if (!result.empty()) {
		return ReturnStatus(context, false, result);
	}

	for (const auto& device : command.devices()) {
		if (!ProcessCmd(context, device, command, false)) {
			return false;
		}
	}

	// ATTACH and DETACH return the device list
	if (context.fd != -1 && (command.operation() == ATTACH || command.operation() == DETACH)) {
		// A new command with an empty device list is required here in order to return data for all devices
		PbCommand command;
		PbResult result;
		rascsi_response.GetDevicesInfo(result, command, devices, UnitNum);
		SerializeMessage(context.fd, result);
		return true;
	}

	return ReturnStatus(context);
}

bool ProcessId(const string id_spec, PbDeviceType type, int& id, int& unit)
{
	size_t separator_pos = id_spec.find(COMPONENT_SEPARATOR);
	if (separator_pos == string::npos) {
		int max_id = type == SAHD ? 16 : 8;

		if (!GetAsInt(id_spec, id) || id < 0 || id >= max_id) {
			cerr << optarg << ": Invalid device ID (0-" << (max_id - 1) << ")" << endl;
			return false;
		}

		// Required for SASI ID/LUN handling backwards compatibility
		unit = 0;
		if (type == SAHD) {
			unit = id % 2;
			id /= 2;
		}
	}
	else {
		int max_unit = type == SAHD ? 2 : UnitNum;

		if (!GetAsInt(id_spec.substr(0, separator_pos), id) || id < 0 || id > 7 ||
				!GetAsInt(id_spec.substr(separator_pos + 1), unit) || unit < 0 || unit >= max_unit) {
			cerr << optarg << ": Invalid unit (0-" << (max_unit - 1) << ")" << endl;
			return false;
		}
	}

	return true;
}

void ShutDown(const CommandContext& context, const string& mode) {
	if (mode.empty()) {
		ReturnLocalizedError(context, ERROR_SHUTDOWN_MODE_MISSING);
		return;
	}

	PbResult result;
	result.set_status(true);

	if (mode == "rascsi") {
		LOGINFO("RaSCSI shutdown requested");

		SerializeMessage(context.fd, result);

		TerminationHandler(0);
	}

	// The root user has UID 0
	if (getuid()) {
		ReturnLocalizedError(context, ERROR_SHUTDOWN_PERMISSION);
		return;
	}

	if (mode == "system") {
		LOGINFO("System shutdown requested");

		SerializeMessage(context.fd, result);

		DetachAll();

		if (system("init 0") == -1) {
			LOGERROR("System shutdown failed: %s", strerror(errno));
		}
	}
	else if (mode == "reboot") {
		LOGINFO("System reboot requested");

		SerializeMessage(context.fd, result);

		DetachAll();

		if (system("init 6") == -1) {
			LOGERROR("System reboot failed: %s", strerror(errno));
		}
	}
	else {
		ReturnLocalizedError(context, ERROR_SHUTDOWN_MODE_INVALID);
	}
}

//---------------------------------------------------------------------------
//
//	Argument Parsing
//
//---------------------------------------------------------------------------
bool ParseArgument(int argc, char* argv[], int& port)
{
	PbCommand command;
	int id = -1;
	int unit = -1;
	PbDeviceType type = UNDEFINED;
	int block_size = 0;
	string name;
	string log_level;

	string locale = setlocale(LC_MESSAGES, "");
	if (locale == "C") {
		locale = "en";
	}

	opterr = 1;
	int opt;
	while ((opt = getopt(argc, argv, "-IiHhb:d:n:p:r:t:z:D:F:L:P:R:")) != -1) {
		switch (opt) {
			// The three options below are kind of a compound option with two letters
			case 'i':
			case 'I':
				id = -1;
				unit = -1;
				continue;

			case 'h':
			case 'H':
				id = -1;
				unit = -1;
				type = SAHD;
				continue;

			case 'd':
			case 'D': {
				if (!ProcessId(optarg, type, id, unit)) {
					return false;
				}
				continue;
			}

			case 'b': {
				if (!GetAsInt(optarg, block_size)) {
					cerr << "Invalid block size " << optarg << endl;
					return false;
				}
				continue;
			}

			case 'z':
				locale = optarg;
				continue;

			case 'F': {
				string result = rascsi_image.SetDefaultImageFolder(optarg);
				if (!result.empty()) {
					cerr << result << endl;
					return false;
				}
				continue;
			}

			case 'L':
				log_level = optarg;
				continue;

			case 'R':
				int depth;
				if (!GetAsInt(optarg, depth) || depth < 0) {
					cerr << "Invalid image file scan depth " << optarg << endl;
					return false;
				}
				rascsi_image.SetDepth(depth);
				continue;

			case 'n':
				name = optarg;
				continue;

			case 'p':
				if (!GetAsInt(optarg, port) || port <= 0 || port > 65535) {
					cerr << "Invalid port " << optarg << ", port must be between 1 and 65535" << endl;
					return false;
				}
				continue;

			case 'P':
				if (!ReadAccessToken(optarg)) {
					return false;
				}
				continue;

			case 'r': {
					string error = SetReservedIds(optarg);
					if (!error.empty()) {
						cerr << error << endl;
						return false;
					}
				}
				continue;

			case 't': {
					string t = optarg;
					transform(t.begin(), t.end(), t.begin(), ::toupper);
					if (!PbDeviceType_Parse(t, &type)) {
						cerr << "Illegal device type '" << optarg << "'" << endl;
						return false;
					}
				}
				continue;

			default:
				return false;

			case 1:
				// Encountered filename
				break;
		}

		if (optopt) {
			return false;
		}

		// Set up the device data
		PbDeviceDefinition *device = command.add_devices();
		device->set_id(id);
		device->set_unit(unit);
		device->set_type(type);
		device->set_block_size(block_size);

		ParseParameters(*device, optarg);

		size_t separator_pos = name.find(COMPONENT_SEPARATOR);
		if (separator_pos != string::npos) {
			device->set_vendor(name.substr(0, separator_pos));
			name = name.substr(separator_pos + 1);
			separator_pos = name.find(COMPONENT_SEPARATOR);
			if (separator_pos != string::npos) {
				device->set_product(name.substr(0, separator_pos));
				device->set_revision(name.substr(separator_pos + 1));
			}
			else {
				device->set_product(name);
			}
		}
		else {
			device->set_vendor(name);
		}

		id = -1;
		type = UNDEFINED;
		block_size = 0;
		name = "";
	}

	if (!log_level.empty() && !SetLogLevel(log_level)) {
		LOGWARN("Invalid log level '%s'", log_level.c_str());
	}

	// Attach all specified devices
	command.set_operation(ATTACH);

	CommandContext context;
	context.fd = -1;
	context.locale = locale;
	if (!ProcessCmd(context, command)) {
		return false;
	}

	// Display and log the device list
	PbServerInfo server_info;
	rascsi_response.GetDevices(server_info, devices);
	const list<PbDevice>& devices = { server_info.devices_info().devices().begin(), server_info.devices_info().devices().end() };
	const string device_list = ListDevices(devices);
	LogDevices(device_list);
	cout << device_list << endl;

	return true;
}

//---------------------------------------------------------------------------
//
//	Pin the thread to a specific CPU
//
//---------------------------------------------------------------------------
void FixCpu(int cpu)
{
	// Get the number of CPUs
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	sched_getaffinity(0, sizeof(cpu_set_t), &cpuset);
	int cpus = CPU_COUNT(&cpuset);

	// Set the thread affinity
	if (cpu < cpus) {
		CPU_ZERO(&cpuset);
		CPU_SET(cpu, &cpuset);
		sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
	}
}

//---------------------------------------------------------------------------
//
//	Monitor Thread
//
//---------------------------------------------------------------------------
static void *MonThread(void *param)
{
    // Scheduler Settings
	struct sched_param schedparam;
	schedparam.sched_priority = 0;
	sched_setscheduler(0, SCHED_IDLE, &schedparam);

	// Set the affinity to a specific processor core
	FixCpu(2);

	// Wait for the execution to start
	while (!running) {
		usleep(1);
	}

	// Set up the monitor socket to receive commands
	listen(monsocket, 1);

	while (true) {
		CommandContext context;
		context.fd = -1;

		try {
			// Wait for connection
			struct sockaddr_in client;
			socklen_t socklen = sizeof(client);
			memset(&client, 0, socklen);
			context.fd = accept(monsocket, (struct sockaddr*)&client, &socklen);
			if (context.fd < 0) {
				throw io_exception("accept() failed");
			}

			// Read magic string
			char magic[6];
			int bytes_read = ReadNBytes(context.fd, (uint8_t *)magic, sizeof(magic));
			if (!bytes_read) {
				continue;
			}
			if (bytes_read != sizeof(magic) || strncmp(magic, "RASCSI", sizeof(magic))) {
				throw io_exception("Invalid magic");
			}

			// Fetch the command
			PbCommand command;
			DeserializeMessage(context.fd, command);

			context.locale = GetParam(command, "locale");
			if (context.locale.empty()) {
				context.locale = "en";
			}

			if (!access_token.empty() && access_token != GetParam(command, "token")) {
				ReturnLocalizedError(context, ERROR_AUTHENTICATION, UNAUTHORIZED);
				continue;
			}

			if (!PbOperation_IsValid(command.operation())) {
				LOGERROR("Received unknown command with operation opcode %d", command.operation());

				ReturnLocalizedError(context, ERROR_OPERATION, UNKNOWN_OPERATION);
				continue;
			}

			LOGTRACE("Received %s command", PbOperation_Name(command.operation()).c_str());

			PbResult result;

			switch(command.operation()) {
				case LOG_LEVEL: {
					string log_level = GetParam(command, "level");
					bool status = SetLogLevel(log_level);
					if (!status) {
						ReturnLocalizedError(context, ERROR_LOG_LEVEL, log_level);
					}
					else {
						ReturnStatus(context);
					}
					break;
				}

				case DEFAULT_FOLDER: {
					string result = rascsi_image.SetDefaultImageFolder(GetParam(command, "folder"));
					if (!result.empty()) {
						ReturnStatus(context, false, result);
					}
					else {
						ReturnStatus(context);
					}
					break;
				}

				case DEVICES_INFO: {
					rascsi_response.GetDevicesInfo(result, command, devices, UnitNum);
					SerializeMessage(context.fd, result);
					break;
				}

				case DEVICE_TYPES_INFO: {
					result.set_allocated_device_types_info(rascsi_response.GetDeviceTypesInfo(result, command));
					SerializeMessage(context.fd, result);
					break;
				}

				case SERVER_INFO: {
					result.set_allocated_server_info(rascsi_response.GetServerInfo(
							result, devices, reserved_ids, current_log_level, GetParam(command, "folder_pattern"),
							GetParam(command, "file_pattern"), rascsi_image.GetDepth()));
					SerializeMessage(context.fd, result);
					break;
				}

				case VERSION_INFO: {
					result.set_allocated_version_info(rascsi_response.GetVersionInfo(result));
					SerializeMessage(context.fd, result);
					break;
				}

				case LOG_LEVEL_INFO: {
					result.set_allocated_log_level_info(rascsi_response.GetLogLevelInfo(result, current_log_level));
					SerializeMessage(context.fd, result);
					break;
				}

				case DEFAULT_IMAGE_FILES_INFO: {
					result.set_allocated_image_files_info(rascsi_response.GetAvailableImages(result,
							GetParam(command, "folder_pattern"), GetParam(command, "file_pattern"),
							rascsi_image.GetDepth()));
					SerializeMessage(context.fd, result);
					break;
				}

				case IMAGE_FILE_INFO: {
					string filename = GetParam(command, "file");
					if (filename.empty()) {
						ReturnLocalizedError(context, ERROR_MISSING_FILENAME);
					}
					else {
						PbImageFile* image_file = new PbImageFile();
						bool status = rascsi_response.GetImageFile(image_file, filename);
						if (status) {
							result.set_status(true);
							result.set_allocated_image_file_info(image_file);
							SerializeMessage(context.fd, result);
						}
						else {
							ReturnStatus(context, false, "Can't get image file info for '" + filename + "'");
						}
					}
					break;
				}

				case NETWORK_INTERFACES_INFO: {
					result.set_allocated_network_interfaces_info(rascsi_response.GetNetworkInterfacesInfo(result));
					SerializeMessage(context.fd, result);
					break;
				}

				case MAPPING_INFO: {
					result.set_allocated_mapping_info(rascsi_response.GetMappingInfo(result));
					SerializeMessage(context.fd, result);
					break;
				}

				case OPERATION_INFO: {
					result.set_allocated_operation_info(rascsi_response.GetOperationInfo(result,
							rascsi_image.GetDepth()));
					SerializeMessage(context.fd, result);
					break;
				}

				case RESERVED_IDS_INFO: {
					result.set_allocated_reserved_ids_info(rascsi_response.GetReservedIds(result, reserved_ids));
					SerializeMessage(context.fd, result);
					break;
				}

				case SHUT_DOWN: {
					ShutDown(context, GetParam(command, "mode"));
					break;
				}

				default: {
					// Wait until we become idle
					while (active) {
						usleep(500 * 1000);
					}

					ProcessCmd(context, command);
					break;
				}
			}
		}
		catch(const io_exception& e) {
			LOGWARN("%s", e.getmsg().c_str());

			// Fall through
		}

		if (context.fd >= 0) {
			close(context.fd);
		}
	}

	return NULL;
}

//---------------------------------------------------------------------------
//
//	Main processing
//
//---------------------------------------------------------------------------
int main(int argc, char* argv[])
{
	GOOGLE_PROTOBUF_VERIFY_VERSION;

#ifndef NDEBUG
	// Get temporary operation info, in order to trigger an assertion on startup if the operation list is incomplete
	PbResult pb_operation_info_result;
	const PbOperationInfo *operation_info = rascsi_response.GetOperationInfo(pb_operation_info_result, 0);
	assert(operation_info->operations_size() == PbOperation_ARRAYSIZE - 1);
	delete operation_info;
#endif

	int actid;
	BUS::phase_t phase;
	// added setvbuf to override stdout buffering, so logs are written immediately and not when the process exits.
	setvbuf(stdout, NULL, _IONBF, 0);
	struct sched_param schparam;

	// Output the Banner
	Banner(argc, argv);

	// ParseArgument() requires the bus to have been initialized first, which requires the root user.
	// The -v option should be available for any user, which requires special handling.
	for (int i = 1 ; i < argc; i++) {
		if (!strcasecmp(argv[i], "-v")) {
			cout << rascsi_get_version_string() << endl;
			return 0;
		}
	}

	SetLogLevel("info");

	// Create a thread-safe stdout logger to process the log messages
	auto logger = stdout_color_mt("rascsi stdout logger");

	int port = 6868;

	if (!InitBus()) {
		return EPERM;
	}

	if (!ParseArgument(argc, argv, port)) {
		Cleanup();
		return -1;
	}

	if (!InitService(port)) {
		return EPERM;
	}

	// Signal handler to detach all devices on a KILL or TERM signal
	struct sigaction termination_handler;
	termination_handler.sa_handler = TerminationHandler;
	sigemptyset(&termination_handler.sa_mask);
	termination_handler.sa_flags = 0;
	sigaction(SIGINT, &termination_handler, NULL);
	sigaction(SIGTERM, &termination_handler, NULL);

	// Reset
	Reset();

    // Set the affinity to a specific processor core
	FixCpu(3);

#ifdef USE_SEL_EVENT_ENABLE
	// Scheduling policy setting (highest priority)
	schparam.sched_priority = sched_get_priority_max(SCHED_FIFO);
	sched_setscheduler(0, SCHED_FIFO, &schparam);
#endif	// USE_SEL_EVENT_ENABLE

	// Start execution
	running = true;

	// Main Loop
	while (running) {
		// Work initialization
		actid = -1;
		phase = BUS::busfree;

#ifdef USE_SEL_EVENT_ENABLE
		// SEL signal polling
		if (bus->PollSelectEvent() < 0) {
			// Stop on interrupt
			if (errno == EINTR) {
				break;
			}
			continue;
		}

		// Get the bus
		bus->Aquire();
#else
		bus->Aquire();
		if (!bus->GetSEL()) {
			usleep(0);
			continue;
		}
#endif	// USE_SEL_EVENT_ENABLE

        // Wait until BSY is released as there is a possibility for the
        // initiator to assert it while setting the ID (for up to 3 seconds)
		if (bus->GetBSY()) {
			int now = SysTimer::GetTimerLow();
			while ((SysTimer::GetTimerLow() - now) < 3 * 1000 * 1000) {
				bus->Aquire();
				if (!bus->GetBSY()) {
					break;
				}
			}
		}

		// Stop because the bus is busy or another device responded
		if (bus->GetBSY() || !bus->GetSEL()) {
			continue;
		}

		pthread_mutex_lock(&ctrl_mutex);

		BYTE data = bus->GetDAT();

		int initiator_id = -1;

		// Notify all controllers
		int i = 0;
		for (auto it = controllers.begin(); it != controllers.end(); ++i, ++it) {
			if (!*it || (data & (1 << i)) == 0) {
				continue;
			}

			// Extract the SCSI initiator ID
			int tmp = data - (1 << i);
			if (tmp) {
				initiator_id = 0;
				for (int j = 0; j < 8; j++) {
					tmp >>= 1;
					if (tmp) {
						initiator_id++;
					}
					else {
						break;
					}
				}
			}

			// Find the target that has moved to the selection phase
			if ((*it)->Process(initiator_id) == BUS::selection) {
				// Get the target ID
				actid = i;

				// Bus Selection phase
				phase = BUS::selection;
				break;
			}
		}

		// Return to bus monitoring if the selection phase has not started
		if (phase != BUS::selection) {
			pthread_mutex_unlock(&ctrl_mutex);
			continue;
		}

		// Start target device
		active = true;

#ifndef USE_SEL_EVENT_ENABLE
		// Scheduling policy setting (highest priority)
		schparam.sched_priority = sched_get_priority_max(SCHED_FIFO);
		sched_setscheduler(0, SCHED_FIFO, &schparam);
#endif	// USE_SEL_EVENT_ENABLE

		// Loop until the bus is free
		while (running) {
			// Target drive
			phase = controllers[actid]->Process(initiator_id);

			// End when the bus is free
			if (phase == BUS::busfree) {
				break;
			}
		}
		pthread_mutex_unlock(&ctrl_mutex);


#ifndef USE_SEL_EVENT_ENABLE
		// Set the scheduling priority back to normal
		schparam.sched_priority = 0;
		sched_setscheduler(0, SCHED_OTHER, &schparam);
#endif	// USE_SEL_EVENT_ENABLE

		// End the target travel
		active = false;
	}

	return 0;
}
