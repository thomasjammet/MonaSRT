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

};

int main(int argc, const char* argv[]) {
	return ServerApp().run(argc, argv);
}
