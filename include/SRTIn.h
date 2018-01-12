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

#if defined(_WIN32) || defined(__WIN32) && !defined(__CYGWIN__) && !defined(WIN32)
	#define WIN32
#endif
#include <srt/srt.h>
#undef LOG_INFO
#undef LOG_DEBUG
#undef min
#undef max

#include "Mona/Thread.h"
#include "Mona/ServerAPI.h"
#include "Mona/TSReader.h"

struct SRTIn : private Mona::Thread {

	SRTIn(const Mona::Parameters& configs, Mona::ServerAPI& api);
	virtual ~SRTIn();

	bool load();
	virtual void stop();

private:

	// Close the socket if created
	void disconnect();

	virtual bool run(Mona::Exception&, const volatile bool& requestStop);

	// Safe-Threaded structure to send TS data to the running publication
	struct TSPacket : Mona::Packet, virtual Mona::Object {
		TSPacket(const Mona::Packet& packet) : Packet(std::move(packet)) {}

	};
	typedef Mona::Event<void(TSPacket&)>	ON(TSPacket);
	typedef Mona::Event<void()>				ON(TSReset);

	static void LogCallback(void* opaque, int level, const char* file, int line, const char* area, const char* message);

	std::string				_host;
	std::string				_name;
	Mona::Publication*		_publication;
	bool					_started;
	Mona::TSReader			_tsReader;

	// members used by thread
	Mona::SocketAddress		_addr;
	Mona::ServerAPI&		_api;
	::SRTSOCKET				_socket;
};
