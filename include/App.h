/*
* This library is free software; you can redistribute it and/or
* modify it under the terms of the GNU Lesser General Public
* License as published by the Free Software Foundation; either
* version 2.1 of the License, or (at your option) any later version.
*
* This library is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
* Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public
* License along with this library; If not, see <http://www.gnu.org/licenses/>
*/

#pragma once

#include "Mona/Mona.h"
#include "Mona/Publication.h"
#include "Mona/Client.h"

namespace Mona {

struct App : virtual Object {

	struct Client : virtual Object {
		Client(Mona::Client& client) : client(client) {}
		virtual ~Client() {}

		virtual void onAddressChanged(const SocketAddress& oldAddress) {}
		virtual bool onInvocation(Exception& ex, const std::string& name, DataReader& arguments, UInt8 responseType) { return false; }
		virtual bool onFileAccess(Exception& ex, Mona::File::Mode mode, Path& file, DataReader& arguments, DataWriter& properties) { return true; }

		virtual bool onPublish(Exception& ex, Publication& publication) { return true; }
		virtual void onUnpublish(Publication& publication) {}

		virtual bool onSubscribe(Exception& ex, const Subscription& subscription, const Publication& publication) { return true; }
		virtual void onUnsubscribe(const Subscription& subscription, const Publication& publication) {}

		Mona::Client& client;
	};

	App(const Parameters& configs) {}


	virtual SocketAddress& onHandshake(const std::string& protocol, const SocketAddress& address, const Parameters& properties, SocketAddress& redirection) { return redirection; }

	virtual App::Client* newClient(Exception& ex, Mona::Client& client, DataReader& parameters, DataWriter& response) {
		return NULL;
	}

	virtual void manage() {}
};

} // namespace Mona
