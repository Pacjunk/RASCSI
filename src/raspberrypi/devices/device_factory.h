//---------------------------------------------------------------------------
//
// SCSI Target Emulator RaSCSI (*^..^*)
// for Raspberry Pi
//
// Copyright (C) 2021 Uwe Seimet
//
// The DeviceFactory singleton creates devices based on their type and the image file extension
//
//---------------------------------------------------------------------------

#pragma once

#include <list>
#include <unordered_set>
#include <unordered_map>
#include <string>
#include "rascsi_interface.pb.h"

using namespace std;
using namespace rascsi_interface;

typedef pair<uint32_t, uint32_t> Geometry;

class Device;

class DeviceFactory
{
private:
	DeviceFactory();
	~DeviceFactory() {}

public:

	static DeviceFactory& instance();

	Device *CreateDevice(PbDeviceType, const string&);
	PbDeviceType GetTypeForFile(const string&) const;
	const unordered_set<uint32_t>& GetSectorSizes(PbDeviceType type) { return sector_sizes[type]; }
	const unordered_set<uint32_t>& GetSectorSizes(const string&);
	const unordered_set<uint64_t> GetCapacities(PbDeviceType) const;
	const unordered_map<string, string>& GetDefaultParams(PbDeviceType type) { return default_params[type]; }
	const list<string> GetNetworkInterfaces() const;
	const unordered_map<string, PbDeviceType> GetExtensionMapping() const { return extension_mapping; }

private:

	unordered_map<PbDeviceType, unordered_set<uint32_t>> sector_sizes;

	// Optional mapping of drive capacities to drive geometries
	unordered_map<PbDeviceType, unordered_map<uint64_t, Geometry>> geometries;

	unordered_map<PbDeviceType, unordered_map<string, string>> default_params;

	unordered_map<string, PbDeviceType> extension_mapping;

	string GetExtension(const string&) const;
};
