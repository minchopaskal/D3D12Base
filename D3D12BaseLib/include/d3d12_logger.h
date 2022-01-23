#pragma once

namespace D3D12 {

enum class LogLevel : int {
	Debug = 0,
	Error,
	Warning,
	Info,
	InfoFancy,

	Count
};

struct Logger {
	/// Set log level. Each log after this call will be printed only if its log level
	/// is below or equal to the one specified here.
	static void setLogLevel(LogLevel lvl);

	/// LogLevel::Error is always logged and LogLevel::Debug is always logged in Debug
	/// and never in Release.
	static void log(LogLevel lvl, const char *fmt, ...);

private:
	static int loggingLevel;
};

}