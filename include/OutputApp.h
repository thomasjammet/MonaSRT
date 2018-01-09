
#pragma once

#include "App.h"
#include "Mona/TSWriter.h"

struct OutputApp : virtual Mona::App {

	struct Client : App::Client, virtual Mona::Object {
		Client(Mona::Client& client, const std::string& host);
		virtual ~Client();

		/* Client implementation */
		virtual void onAddressChanged(const Mona::SocketAddress& oldAddress) {}
		virtual bool onInvocation(Mona::Exception& ex, const std::string& name, Mona::DataReader& arguments, Mona::UInt8 responseType) { return true; }
		virtual bool onFileAccess(Mona::Exception& ex, Mona::File::Mode mode, Mona::Path& file, Mona::DataReader& arguments, Mona::DataWriter& properties) { return true; }

		virtual bool onPublish(Mona::Exception& ex, Mona::Publication& publication);
		virtual void onUnpublish(Mona::Publication& publication);

		virtual bool onSubscribe(Mona::Exception& ex, const Mona::Subscription& subscription, const Mona::Publication& publication) { return true; }
		virtual void onUnsubscribe(const Mona::Subscription& subscription, const Mona::Publication& publication) {}
	
	private:
		// Inject the TS buffer into SRT 
		// return False if an error occurs, True otherwise
		bool writePayload(Mona::UInt16 context, std::shared_ptr<Mona::Buffer>& pBuffer);

		// Reset the SRT connection
		void resetSRT();

		// FLV
		Mona::TSWriter								_tsWriter;
		Mona::Packet								_videoCodec; // video codec to be saved
		bool										_videoCodecSent;
		std::unique_ptr<Mona::Media::Video::Tag>	_videoTag; // video tag to be saved
		Mona::Packet								_audioCodec; // audio codec to be saved
		bool										_audioCodecSent;
		std::unique_ptr<Mona::Media::Audio::Tag>	_audioTag; // audio tag to be saved
		bool										_first; // To write the FLV header when the first packet is written

		std::string									_host; // host address to connect to (and bind to)
		Mona::Publication::OnAudio					_onAudio;
		Mona::Publication::OnVideo					_onVideo;
		Mona::Publication::OnEnd					_onEnd;
		Mona::Publication*							_pPublication;
		
		class OpenSrtPIMPL;
		std::unique_ptr<OpenSrtPIMPL> _srtPimpl;
	};

	OutputApp(const Mona::Parameters& configs);
	virtual ~OutputApp();

	virtual void onHandshake(const std::string& protocol, const Mona::SocketAddress& address, const Mona::Parameters& properties, std::set<Mona::SocketAddress>& addresses) {}

	virtual OutputApp::Client* newClient(Mona::Exception& ex, Mona::Client& client, Mona::DataReader& parameters, Mona::DataWriter& response);

	virtual void manage() {}
private:
	std::string _target;
};
