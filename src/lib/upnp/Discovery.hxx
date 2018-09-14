/*
 * Copyright 2003-2017 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef _UPNPPDISC_H_X_INCLUDED_
#define _UPNPPDISC_H_X_INCLUDED_

#include "Compat.hxx"
#include "Callback.hxx"
#include "Device.hxx"
#include "WorkQueue.hxx"
#include "thread/Mutex.hxx"
#include "Compiler.h"

#include <upnp.h>

#include <list>
#include <vector>
#include <string>
#include <memory>
#include <chrono>

#if UPNP_VERSION < 10800
#define UpnpDiscovery Upnp_Discovery
#endif

class ContentDirectoryService;

class UPnPDiscoveryListener {
public:
	virtual void FoundUPnP(const ContentDirectoryService &service) = 0;
	virtual void LostUPnP(const ContentDirectoryService &service) = 0;
};

/**
 * Manage UPnP discovery and maintain a directory of active devices. Singleton.
 *
 * We are only interested in MediaServers with a ContentDirectory service
 * for now, but this could be made more general, by removing the filtering.
 */
class UPnPDeviceDirectory final : UpnpCallback {
	/**
	 * Each appropriate discovery event (executing in a libupnp thread
	 * context) queues the following task object for processing by the
	 * discovery thread.
	 */
	struct DiscoveredTask {
		std::string url;
		std::string device_id;
		std::chrono::steady_clock::duration expires;

		DiscoveredTask(const UpnpDiscovery *disco)
			:url(UpnpDiscovery_get_Location_cstr(disco)),
			 device_id(UpnpDiscovery_get_DeviceID_cstr(disco)),
			 expires(std::chrono::seconds(UpnpDiscovery_get_Expires(disco))) {}
	};

	/**
	 * Descriptor for one device having a Content Directory
	 * service found on the network.
	 */
	class ContentDirectoryDescriptor {
	public:
		std::string id;

		UPnPDevice device;

		/**
		 * The time stamp when this device expires.
		 */
		std::chrono::steady_clock::time_point expires;

		ContentDirectoryDescriptor() = default;

		ContentDirectoryDescriptor(std::string &&_id,
					   std::chrono::steady_clock::time_point last,
					   std::chrono::steady_clock::duration exp)
			:id(std::move(_id)),
			 expires(last + exp + std::chrono::seconds(20)) {}

		void Parse(const std::string &url, const char *description) {
			device.Parse(url, description);
		}
	};

	const UpnpClient_Handle handle;
	UPnPDiscoveryListener *const listener;

	Mutex mutex;
	std::list<ContentDirectoryDescriptor> directories;
	WorkQueue<std::unique_ptr<DiscoveredTask>> queue;

	/**
	 * The UPnP device search timeout, which should actually be
	 * called delay because it's the base of a random delay that
	 * the devices apply to avoid responding all at the same time.
	 */
	int search_timeout = 2;

	/**
	 * The time stamp of the last search.
	 */
	std::chrono::steady_clock::time_point last_search = std::chrono::steady_clock::time_point();

public:
	UPnPDeviceDirectory(UpnpClient_Handle _handle,
			    UPnPDiscoveryListener *_listener=nullptr);
	~UPnPDeviceDirectory();

	UPnPDeviceDirectory(const UPnPDeviceDirectory &) = delete;
	UPnPDeviceDirectory& operator=(const UPnPDeviceDirectory &) = delete;

	void Start();

	/** Retrieve the directory services currently seen on the network */
	std::vector<ContentDirectoryService> GetDirectories();

	/**
	 * Get server by friendly name.
	 */
	ContentDirectoryService GetServer(const char *friendly_name);

private:
	void Search();

	/**
	 * Look at the devices and get rid of those which have not
	 * been seen for too long. We do this when listing the top
	 * directory.
	 *
	 * Caller must lock #mutex.
	 */
	void ExpireDevices();

	void LockAdd(ContentDirectoryDescriptor &&d);
	void LockRemove(const std::string &id);

	/**
	 * Worker routine for the discovery queue. Get messages about
	 * devices appearing and disappearing, and update the
	 * directory pool accordingly.
	 */
	static void *Explore(void *);
	void Explore();

	int OnAlive(const UpnpDiscovery *disco);
	int OnByeBye(const UpnpDiscovery *disco);

	/* virtual methods from class UpnpCallback */
	virtual int Invoke(Upnp_EventType et, const void *evp) override;
};


#endif /* _UPNPPDISC_H_X_INCLUDED_ */
