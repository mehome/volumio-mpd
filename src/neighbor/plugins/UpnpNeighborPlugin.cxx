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

#include "config.h"
#include "UpnpNeighborPlugin.hxx"
#include "lib/upnp/ClientInit.hxx"
#include "lib/upnp/Discovery.hxx"
#include "lib/upnp/ContentDirectoryService.hxx"
#include "neighbor/NeighborPlugin.hxx"
#include "neighbor/Explorer.hxx"
#include "neighbor/Listener.hxx"
#include "neighbor/Info.hxx"
#include "Log.hxx"

#include <stdexcept>

class UpnpNeighborExplorer final
	: public NeighborExplorer, UPnPDiscoveryListener {
	struct Server {
		std::string name, comment;

		bool alive;

		Server(std::string &&_name, std::string &&_comment)
			:name(std::move(_name)), comment(std::move(_comment)),
			 alive(true) {}
		Server(const Server &) = delete;

		gcc_pure
		bool operator==(const Server &other) const noexcept {
			return name == other.name;
		}

		gcc_pure
		NeighborInfo Export() const noexcept {
			return { "smb://" + name + "/", comment };
		}
	};

	UPnPDeviceDirectory *discovery;

public:
	UpnpNeighborExplorer(NeighborListener &_listener)
		:NeighborExplorer(_listener) {}

	/* virtual methods from class NeighborExplorer */
	void Open() override;
	virtual void Close() override;
	virtual List GetList() const override;

private:
	/* virtual methods from class UPnPDiscoveryListener */
	virtual void FoundUPnP(const ContentDirectoryService &service) override;
	virtual void LostUPnP(const ContentDirectoryService &service) override;
};

void
UpnpNeighborExplorer::Open()
{
	UpnpClient_Handle handle;
	UpnpClientGlobalInit(handle);

	discovery = new UPnPDeviceDirectory(handle, this);

	try {
		discovery->Start();
	} catch (...) {
		delete discovery;
		UpnpClientGlobalFinish();
		throw;
	}
}

void
UpnpNeighborExplorer::Close()
{
	delete discovery;
	UpnpClientGlobalFinish();
}

NeighborExplorer::List
UpnpNeighborExplorer::GetList() const
{
	std::vector<ContentDirectoryService> tmp;

	try {
		tmp = discovery->GetDirectories();
	} catch (const std::runtime_error &e) {
		LogError(e);
	}

	List result;
	for (const auto &i : tmp)
		result.emplace_front(i.GetURI(), i.getFriendlyName());
	return result;
}

void
UpnpNeighborExplorer::FoundUPnP(const ContentDirectoryService &service)
{
	const NeighborInfo n(service.GetURI(), service.getFriendlyName());
	listener.FoundNeighbor(n);
}

void
UpnpNeighborExplorer::LostUPnP(const ContentDirectoryService &service)
{
	const NeighborInfo n(service.GetURI(), service.getFriendlyName());
	listener.LostNeighbor(n);
}

static NeighborExplorer *
upnp_neighbor_create(gcc_unused EventLoop &loop,
		     NeighborListener &listener,
		     gcc_unused const ConfigBlock &block)
{
	return new UpnpNeighborExplorer(listener);
}

const NeighborPlugin upnp_neighbor_plugin = {
	"upnp",
	upnp_neighbor_create,
};
