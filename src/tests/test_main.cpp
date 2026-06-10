// SPDX-License-Identifier: Apache-2.0
//
// Catch2 entry point for the CoroTest binary. Custom main so future
// work can register CLI flags for tests if needed.

#define CATCH_CONFIG_RUNNER
#include <catch2/catch_session.hpp>

int main(int argc, char* argv[])
{
    auto session = Catch::Session {};
    auto const cliReturn = session.applyCommandLine(argc, argv);
    if (cliReturn != 0)
        return cliReturn;
    return session.run();
}
