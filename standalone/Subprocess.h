#pragma once

#include <functional>
#include <string>
#include <vector>

namespace nam_ui
{

/// Quotes one argument for a Windows command line. Paths to music have spaces, brackets and
/// apostrophes in them far more often than not.
std::wstring QuoteArgument(const std::wstring& value);

std::wstring ToWide(const std::string& text);

/// \brief Runs the first of several commands that starts, and reads what it prints.
///
/// The tools this drives are Python programs, and on Windows pip routinely installs those into a
/// Scripts folder that is not on PATH - so "demucs" alone fails on a machine where it is perfectly
/// well installed. Hence a list of ways to reach the same thing rather than one command.
///
/// stdout and stderr go into one pipe: a progress bar goes to one and everything else to the
/// other, and for showing the last line it makes no difference which is which. Carriage returns
/// end a line as well as newlines, because that is how a progress bar redraws itself.
///
/// Blocks until the process exits. Meant for a worker thread.
///
/// \param candidates what to try, in order. One containing a space is a program plus arguments and
///        goes through as written; one without is a bare name or path and gets quoted.
/// \param arguments appended to whichever candidate starts, already quoted
/// \param onLine called for each line the process writes. May be empty.
/// \param error set to something worth showing a person when nothing would start
/// \return the process's exit code, or -1 when none of the candidates started
int RunProcess(const std::vector<std::string>& candidates, const std::wstring& arguments,
               const std::function<void(const std::string&)>& onLine, std::string& error);

} // namespace nam_ui
