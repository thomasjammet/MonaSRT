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

#include "Mona/Server.h"
#include "Mona/ServerApplication.h"
#include "MonaSRT.h"
#include "Version.h"
#include "MonaSRTVersion.h"

#define VERSION \
	"1." STRINGIZE(MONA_VERSION) \
	"-srt-" \
	STRINGIZE(MONASRT_VERSION)

using namespace std;
using namespace Mona;

struct ServerApp : ServerApplication  {

	ServerApp () : ServerApplication() {
		setBoolean("HTTP", false);
		setBoolean("HTTPS", false);
		setBoolean("RTMFP", false);
		setString("logs.maxsize", "50000000");
	}

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
