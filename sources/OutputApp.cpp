#include "OutputApp.h"
#include "Mona/String.h"
#include "Mona/AVC.h"
//#include "srt.h" ?

using namespace Mona;
using namespace std;

OutputApp::OutputApp(const Parameters& configs) : App(configs) {

}

OutputApp::~OutputApp() {
}

OutputApp::Client::Client(Mona::Client& client, const string& host) : App::Client(client), _host(host), 
	_first(true), _pPublication(NULL), _videoCodecSent(false), _audioCodecSent(false) {
	_onAudio = [this](UInt16 track, const Media::Audio::Tag& tag, const Packet& packet) {
		/*if (_stopping)
			return resetSRT();
		*/

		// AAC codecs to be sent in first
		if (!_audioCodecSent) {
			if (tag.codec == Media::Audio::CODEC_AAC && tag.isConfig) {

				INFO("AAC codec infos saved")
				_audioCodec.set(std::move(packet));
				_audioTag.reset(new Media::Audio::Tag(tag));
			}
			if (!_audioCodec)
				return;

			_audioCodecSent = true;
			INFO("AAC codec infos sent")
			if (!tag.isConfig) {
				shared<Buffer> pBuffer(new Buffer());
				BinaryWriter writer(*pBuffer);
				if (_first) {
					_tsWriter.beginMedia([&writer](const Packet& output) { writer.write(output); });
					_first = false;
				}
				_tsWriter.writeAudio(0, *_audioTag, _audioCodec, [&writer](const Packet& output) { writer.write(output); });
				if (!writePayload(0, pBuffer)) {
					//_stopping = true;
					return;
				}
			}
		}

		shared<Buffer> pBuffer(new Buffer());
		BinaryWriter writer(*pBuffer);
		if (_first) {
			_tsWriter.beginMedia([&writer](const Packet& output) { writer.write(output); });
			_first = false;
		}
		_tsWriter.writeAudio(0, tag, packet, [&writer](const Packet& output) { writer.write(output); });
		if (!writePayload(0, pBuffer)) {
			//_stopping = true;
			return;
		}
	};
	_onVideo = [this](UInt16 track, const Media::Video::Tag& tag, const Packet& packet) {
		/*if (_stopping)
			return resetSRT();*/

		// Video codecs to be sent in first
		if (!_videoCodecSent) {

			Packet sps, pps;
			bool isAVCConfig(tag.codec == Media::Video::CODEC_H264 && tag.frame == Media::Video::FRAME_CONFIG && AVC::ParseVideoConfig(packet, sps, pps));
			if (isAVCConfig) {
				INFO("Video codec infos saved")
				_videoCodec.set(std::move(packet));
				_videoTag.reset(new Media::Video::Tag(tag));
			}
			if (!_videoCodec)
				return;

			if (tag.frame != Media::Video::FRAME_KEY) {
				DEBUG("Video frame dropped to wait first key frame")
				return;
			}

			_videoCodecSent = true;
			INFO("Video codec infos sent")
			if (!isAVCConfig) {
				shared<Buffer> pBuffer(new Buffer());
				BinaryWriter writer(*pBuffer);
				if (_first) {
					_tsWriter.beginMedia([&writer](const Packet& output) { writer.write(output); });
					_first = false;
				}
				_tsWriter.writeVideo(0, *_videoTag, _videoCodec, [&writer](const Packet& output) { writer.write(output); });
				if (!writePayload(0, pBuffer)) {
					//_stopping = true;
					return;
				}
			}
		}

		shared<Buffer> pBuffer(new Buffer());
		BinaryWriter writer(*pBuffer);
		if (_first) {
			_tsWriter.beginMedia([&writer](const Packet& output) { writer.write(output); });
			_first = false;
		}
		_tsWriter.writeVideo(0, tag, packet, [&writer](const Packet& output) { writer.write(output); });
		if (!writePayload(0, pBuffer)) {
			//_stopping = true;
			return;
		}
	};
	_onEnd = [this]() {
		resetSRT();
	};
	INFO("A new publish client is connecting from ", client.address);
}

OutputApp::Client::~Client() {
	INFO("Client from ", client.address, " is disconnecting...")
	resetSRT();
}

bool OutputApp::Client::onPublish(Exception& ex, Publication& publication) {
	INFO("Client from ", client.address, " is trying to publish ", publication.name())

	if (_pPublication) {
		WARN("Client is already publishing, request ignored")
		return false;
	}

	// Init parameters
	publication.onAudio = _onAudio;
	publication.onVideo = _onVideo;
	publication.onEnd = _onEnd;
	_pPublication = &publication;

	// Start SRT Publication
	/* INSERT CODE HERE */

	return true;
}

void OutputApp::Client::onUnpublish(Publication& publication) {
	INFO("Client from ", client.address, " has closed publication ", publication.name(), ", stopping the injection...")

	resetSRT();
}

void OutputApp::Client::resetSRT() {

	if (_pPublication) {
		_pPublication->onAudio = nullptr;
		_pPublication->onVideo = nullptr;
		_pPublication->onEnd = nullptr;
		_pPublication = NULL;
	}

	_tsWriter.endMedia([](const Packet& packet) {}); // reset the ts writer
	_videoCodec.reset();
	_videoTag.reset();
	_audioCodec.reset();
	_audioTag.reset();
	_first = false;
}

bool OutputApp::Client::writePayload(UInt16 context, shared_ptr<Buffer>& pBuffer) {

	int res = 0; // SRT_Write(pBuffer->data(), pBuffer->size());
	/* INSERT CODE HERE */

	return res != -1;
}

OutputApp::Client* OutputApp::newClient(Mona::Exception& ex, Mona::Client& client, Mona::DataReader& parameters, Mona::DataWriter& response) {

	return new Client(client, "");
}
