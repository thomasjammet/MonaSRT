#include "Mona/Server.h"
#include "Mona/ServerApplication.h"
#include "MonaSRT.h"
#include "Version.h"

#define VERSION		"1." STRINGIZE(MONA_VERSION)

using namespace std;
using namespace Mona;

struct ServerApp : ServerApplication  {

	const char* defineVersion() { return VERSION; }

///// MAIN
	int main(TerminateSignal& terminateSignal) {

		// starts the server
		MonaSRT server(file().parent()+"www", getNumber<UInt32>("cores"), terminateSignal);

		if (server.start(*this)) {

			terminateSignal.wait();
			// Stop the server
			server.stop();
		}
		return Application::EXIT_OK;
	}

	void defineOptions(Exception& ex, Options& options)
	{
		options.add(ex, "srttarget", "st", "Specify SRT target.")
			.argument("<host>:<port>")
			.handler([this](Exception& ex, const string& value) { 
				setString("srt.target", value);
				return true; });

		ServerApplication::defineOptions(ex, options);
	}
private:
	std::string _target;
};

int main(int argc, const char* argv[]) {
	return ServerApp().run(argc, argv);
}
