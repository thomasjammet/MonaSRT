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



#include "SRTIn.h"
/*#include "Mona/String.h"
#include "Mona/AVC.h"
#include "Mona/SocketAddress.h"*/

using namespace Mona;
using namespace std;

static const int EpollWaitTimoutMS = 250;
static const int TSChunkSize = 1316;

SRTIn::SRTIn(const Parameters& configs, ServerAPI& api): Thread("SRTIn"), _api(api), _started(false), _socket(::SRT_INVALID_SOCK) {
	onTSPacket = [this](TSPacket& obj) {
		_tsReader.read(obj, *_publication);
	};
	onTSReset = [this]() {
		_tsReader.flush(*_publication);
	};

	_host.assign(configs.getString("srt.host", "0.0.0.0:1234"));
	_name.assign(configs.getString("srt.name", "srtIn"));
}

SRTIn::~SRTIn() {

	stop();
}

void SRTIn::stop() {
	
	Thread::stop();

	if (_started) {
		::srt_setloghandler(nullptr, nullptr);
		::srt_cleanup();
		_started = false;
	}

	if (_publication) {
		_api.unpublish(*_publication);
		_publication = nullptr;
	}
}

void SRTIn::disconnect() {
	if (_socket == ::SRT_INVALID_SOCK)
		return;
		
	::srt_close(_socket);
	INFO("SRT disconnect state; ", ::srt_getsockstate(_socket));
	_socket = ::SRT_INVALID_SOCK;
}

bool SRTIn::load() {

	if (_started) {
		ERROR("SRTIn load: Already open, please close first")
		return false;
	}

	if (::srt_startup()) {
		ERROR("SRTIn load: Error starting SRT library")
		return false;
	}
	_started = true;

	::srt_setloghandler(nullptr, LogCallback);
	::srt_setloglevel(0xff);

	Exception ex;
	if (!_addr.set(ex, _host) || _addr.family() != IPAddress::IPv4) {
		ERROR("SRTIn load: can't resolve target host, ", _host)
		stop();
		return false;
	}

	if (!(_publication = _api.publish(ex, _name))) {
		ERROR("SRT publish: ", ex)
		stop();
		return false;
	}

	if (!Thread::start(ex)) {
		ERROR("SRT Open: can't start monitor thread")
		stop();
		return false;
	}

	return true;
}

bool SRTIn::run(Exception&, const volatile bool& requestStop) {
	NOTE("Starting SRT server on host ", _host)

	_socket = ::srt_socket(AF_INET, SOCK_DGRAM, 0);
	if (_socket == ::SRT_INVALID_SOCK) {
		ERROR("SRTIn create socket: ", ::srt_getlasterror_str());
		return false;
	}

	bool block = false;
	if (::srt_setsockopt(_socket, 0, SRTO_RCVSYN, &block, sizeof(block)) != 0) {
		disconnect();
		ERROR("SRTIn SRTO_SNDSYN: ", ::srt_getlasterror_str());
		return false;
	}

	::SRT_SOCKSTATUS state = ::srt_getsockstate(_socket);
	if (state != SRTS_INIT) {
		ERROR("SRTIn Connect: socket is in bad state; ", state)
		disconnect();
		return false;
	}

	INFO("Binding ", _addr.host(), " port ", _addr.port())

	// SRT support only IPV4 so we convert to a sockaddr_in
	sockaddr addr;
	memcpy(&addr, _addr.data(), sizeof(sockaddr)); // WARN: work only with ipv4 addresses
	addr.sa_family = AF_INET;
	if (::srt_bind(_socket, &addr, sizeof(sockaddr))) {
		ERROR("SRTIn Bind: ", ::srt_getlasterror_str());
		disconnect();
		return false;
	}

	if (::srt_listen(_socket, 1)) {
		ERROR("SRTIn Listen: ", ::srt_getlasterror_str());
		disconnect();
		return false;
	}

	int epollid = ::srt_epoll_create();
	if (epollid < 0) {
		ERROR("Error initializing UDT epoll set;", ::srt_getlasterror_str());
		disconnect();
		return false;
	}

	int modes = SRT_EPOLL_IN;
	::srt_epoll_add_usock(epollid, _socket, &modes);

	// Accept 1 connection at a time
	while (!requestStop) {

		// Wait for a connection
		const int socksToPoll = 10;
		int rfdn = socksToPoll;
		::SRTSOCKET rfds[socksToPoll];
		int rc = 0;
		::SRTSOCKET newSocket;
		for (;;) {

			if ((rc = ::srt_epoll_wait(epollid, &rfds[0], &rfdn, nullptr, nullptr, EpollWaitTimoutMS, nullptr, nullptr, nullptr, nullptr)) > 0) {

				sockaddr_in scl;
				int sclen = sizeof scl;
				newSocket = ::srt_accept(_socket, (sockaddr*)&scl, &sclen);
				if (newSocket == SRT_INVALID_SOCK) {
					ERROR("SRTIn epoll wait: ", ::srt_getlasterror_str());
					disconnect();
					return false;
				}
				INFO("Connection from ", SocketAddress(*((sockaddr*)(&scl))))
				break;
			}

			// ETIMEOUT is not an error
			if (::srt_getlasterror(NULL) != SRT_ETIMEOUT) {
				ERROR("SRTIn epoll wait: ", ::srt_getlasterror_str());
				disconnect();
				return false;
			}

			if (requestStop) {
				disconnect();
				return true;
			}
		}

		// Create the new socket for the SRT publisher
		bool blocking = true;
		if (::srt_setsockopt(newSocket, 0, SRTO_RCVSYN, &blocking, sizeof blocking) == -1) {
			ERROR("SRTIn SRTO_RCVSYN: ", ::srt_getlasterror_str());
			disconnect();
			return false;
		}

		int epollid2 = ::srt_epoll_create();
		if (epollid < 0) {
			ERROR("Error initializing UDT epoll set;", ::srt_getlasterror_str());
			disconnect();
			return false;
		}

		modes = SRT_EPOLL_IN;
		::srt_epoll_add_usock(epollid2, newSocket, &modes);

		// Accept Input until an error is received
		while (!requestStop && epollid2 >= 0) {
			shared<Buffer> pBuffer(new Buffer(TSChunkSize));
			bool ready = true;
			int stat;
			do {
				stat = ::srt_recvmsg(newSocket, STR pBuffer->data(), TSChunkSize);
				if (stat == SRT_ERROR) {

					// EAGAIN for SRT READING
					int error = ::srt_getlasterror(NULL);
					if (error == SRT_EASYNCRCV) {

						// Poll on this descriptor until reading is available, indefinitely.
						int len = 2;
						::SRTSOCKET ready[2];
						if ((stat = ::srt_epoll_wait(epollid2, ready, &len, 0, 0, EpollWaitTimoutMS, 0, 0, 0, 0)) != -1) {
							stat = 0;
							continue;
						}
						// If was -1, then passthru.
					}
					else if (error != ::SRT_ECONNLOST) // not an error
						ERROR("SRTIn recvmsg : ", ::srt_getlasterror_str())
					break;
				}

				if (stat == 0) {
					// Not necessarily eof. Closed connection is reported as error.
					this_thread::sleep_for(chrono::milliseconds(10));
					ready = false;
				}
			} while (!ready && stat >= 0);

			// Error received, we stop reading
			if (stat < 0)
				break;

			// Data found => we push it to the publication
			if (ready && stat > 0) {
				if ((UInt32)stat < pBuffer->size())
					pBuffer->resize(stat);

				// Push TS data to the publication (switch thread to main thread)
				_api.handler.queue(onTSPacket, Packet(pBuffer));
			}
		} // while (!requestStop && epollid2 >= 0)

		// Reset the TS reader (switch thread to main thread)
		_api.handler.queue(onTSReset);

		// Destroy the publisher socket
		::srt_epoll_remove_usock(epollid2, newSocket);

		// Release epoll id
		if (epollid2 > 0)
			::srt_epoll_release(epollid2);

	} // while (!requestStop)

	INFO("End of SRTIn process")

	// Release epoll id
	if (epollid > 0)
		::srt_epoll_release(epollid);

	return true;
}


void SRTIn::LogCallback(void* opaque, int level, const char* file, int line, const char* area, const char* message) {
	if (level != 7)
		INFO("L:", level, "|", file, "|", line, "|", area, "|", message)
}
