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
		_tsReader.read(obj.packet, *_publication);
	};

	_host.assign(configs.getString("srt.host", "0.0.0.0:4900"));
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

	_mutex.lock();

	_socket = ::srt_socket(AF_INET, SOCK_DGRAM, 0);
	if (_socket == ::SRT_INVALID_SOCK) {
		ERROR("SRTIn create socket: ", ::srt_getlasterror_str());
		return false;
	}

	bool block = false;
	if (::srt_setsockopt(_socket, 0, SRTO_SNDSYN, &block, sizeof(block)) != 0) {
		disconnect();
		ERROR("SRTIn SRTO_SNDSYN: ", ::srt_getlasterror_str());
		return false;
	}
	if (::srt_setsockopt(_socket, 0, SRTO_RCVSYN, &block, sizeof(block)) != 0) {
		ERROR("SRTIn SRTO_RCVSYN: ", ::srt_getlasterror_str());
		disconnect();
		return false;
	}

	int opt = 1;
	::srt_setsockflag(_socket, ::SRTO_SENDER, &opt, sizeof opt);

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

	/*
	if (!m_blocking_mode){
		srt_conn_epoll = AddPoller(m_bindsock, SRT_EPOLL_OUT);
	}

	sockaddr_in sa = CreateAddrInet(host, port);
	sockaddr* psa = (sockaddr*)&sa;
	if (transmit_verbose)
	{
		cout << "Binding a server on " << host << ":" << port << " ...";
		cout.flush();
	}*/

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

	/*int len = 2;
	SRTSOCKET ready[2];
	if (::srt_epoll_wait(epollid, 0, 0, ready, &len, -1, 0, 0, 0, 0) == -1) {
		ERROR("SRTIn epoll wait: ", ::srt_getlasterror_str());
		disconnect();
		return false;
	}

	m_sock = srt_accept(_socket, (sockaddr*)&scl, &sclen);
	if (m_sock == SRT_INVALID_SOCK) {
		ERROR("SRTIn epoll wait: ", ::srt_getlasterror_str());
		disconnect();
		return false;
	}


	// ConfigurePre is done on bindsock, so any possible Pre flags
	// are DERIVED by sock. ConfigurePost is done exclusively on sock.
	stat = ConfigurePost(m_sock);
	if (stat == SRT_ERROR)
		Error(UDT::getlasterror(), "ConfigurePost");*/

	shared<Buffer> pBuffer;
	while (!requestStop && epollid >= 0) {
		pBuffer.reset(new Buffer(TSChunkSize));
		bool ready = true;
		int stat;
		do {
			//::throw_on_interrupt = true;
			stat = srt_recvmsg(_socket, STR pBuffer->data(), TSChunkSize);
			//::throw_on_interrupt = false;
			if (stat == SRT_ERROR) {

				// EAGAIN for SRT READING
				if (srt_getlasterror(NULL) == SRT_EASYNCRCV) {

					// Poll on this descriptor until reading is available, indefinitely.
					int len = 2;
					SRTSOCKET ready[2];
					if (srt_epoll_wait(epollid, ready, &len, 0, 0, EpollWaitTimoutMS, 0, 0, 0, 0) != -1) {
						continue;
					}
					// If was -1, then passthru.
				}
				ERROR("SRTIn recvmsg : ", ::srt_getlasterror_str())
				break;
			}

			if (stat == 0) {
				// Not necessarily eof. Closed connection is reported as error.
				this_thread::sleep_for(chrono::milliseconds(10));
				ready = false;
			}
		} while (!ready);

		if (ready && stat > 0) {
			if ((UInt32)stat < pBuffer->size())
				pBuffer->resize(stat);

			writeData(pBuffer);
		}
	}

	

	/*while (!requestStop && epollid >= 0) {
		::SRT_SOCKSTATUS state = ::srt_getsockstate(_socket);
		if (state == ::SRTS_BROKEN || state == ::SRTS_NONEXIST
			|| state == ::SRTS_CLOSED) {
			INFO("Reconnect socket");
			if (_socket != ::SRT_INVALID_SOCK) {
				DEBUG("Remove socket from poll; ", (int)_socket);
				::srt_epoll_remove_usock(epollid, _socket);
			}

			disconnect();
			if (!ConnectActual()) {

				ERROR("Error issuing connect");
				break;
			}

			FATAL_CHECK(_socket != ::SRT_INVALID_SOCK)
				int modes = SRT_EPOLL_IN;
			if (::srt_epoll_add_usock(epollid, _socket, &modes) != 0) {
				ERROR("Error adding socket to poll set; ",
					::srt_getlasterror_str());
				break;
			}
		}

		_mutex.unlock();
		const int socksToPoll = 10;
		int rfdn = socksToPoll;
		::SRTSOCKET rfds[socksToPoll];
		int rc = ::srt_epoll_wait(epollid, &rfds[0], &rfdn, nullptr, nullptr,
			epollWaitTimoutMS, nullptr, nullptr, nullptr, nullptr);

		if (rc <= 0) {
			// Let the system breath just in case
			Sleep(0);

			_mutex.lock();
			continue;
		}
		_mutex.lock();

		FATAL_CHECK(rfdn <= socksToPoll)

			for (int i = 0; i < rfdn; i++) {
				::SRTSOCKET socket = rfds[i];
				state = ::srt_getsockstate(socket);
				switch (state) {
				case ::SRTS_CONNECTED: {
					// Discard incoming data
					static char buf[1500];
					while (::srt_recvmsg(socket, &buf[0], sizeof(buf)) > 0)
						continue;
				}
									   break;
				case ::SRTS_NONEXIST:
				case ::SRTS_BROKEN:
				case ::SRTS_CLOSING:
				case ::SRTS_CLOSED: {
					DEBUG("Remove socket from poll (on poll event); ", socket);
					::srt_epoll_remove_usock(epollid, socket);
				}
									break;
				default: {
					WARN("Unexpected event on ", socket, "state ", state);
				}
						 break;
				}
			}
	}*/

	// TODO: debug this
	if ((false) && epollid > 0)
		::srt_epoll_release(epollid);

	_mutex.unlock();
	return true;
}

void SRTIn::writeData(shared<Buffer>& pBuffer) {

	_api.handler.queue(onTSPacket, Packet(pBuffer));
}


void SRTIn::LogCallback(void* opaque, int level, const char* file, int line, const char* area, const char* message) {
	INFO("L:", level, "|", file, "|", line, "|", area, "|", message)
}
