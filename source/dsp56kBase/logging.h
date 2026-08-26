#pragma once

#include <sstream>
#include <string>
#include <iomanip>
#include <cstdint>

namespace Logging
{
	typedef void (*LogFunc)(const std::string&);

	void g_logToConsole( const std::string& _s );
	void g_logToFile( const std::string& _s );
	void setLogFunc(LogFunc _func);

	inline thread_local uint32_t g_currentThreadSuppressionDepth = 0;

	inline bool isCurrentThreadLogEnabled() noexcept
	{
		return g_currentThreadSuppressionDepth == 0;
	}

	class ScopedCurrentThreadLogSuppression final
	{
	public:
		ScopedCurrentThreadLogSuppression() noexcept
		{
			++g_currentThreadSuppressionDepth;
		}

		~ScopedCurrentThreadLogSuppression()
		{
			--g_currentThreadSuppressionDepth;
		}

		ScopedCurrentThreadLogSuppression(
			const ScopedCurrentThreadLogSuppression&) = delete;
		ScopedCurrentThreadLogSuppression& operator=(
			const ScopedCurrentThreadLogSuppression&) = delete;
	};
}

#define LOGTOCONSOLE(ss)	{ Logging::g_logToConsole( (ss).str() ); }
#define LOGTOFILE(ss)		{ Logging::g_logToFile( (ss).str() ); }

#define LOG(S)																												\
do																															\
{																															\
	if(!Logging::isCurrentThreadLogEnabled()) break;													\
	std::stringstream __ss__logging_h;	__ss__logging_h << __func__ << "@" << __LINE__ << ": " << S;						\
																															\
	LOGTOCONSOLE(__ss__logging_h)																							\
}																															\
while(false)

#define LOGF(S)																												\
do																															\
{																															\
	if(!Logging::isCurrentThreadLogEnabled()) break;													\
	std::stringstream __ss__logging_h;	__ss__logging_h << S;																\
																															\
	LOGTOFILE(__ss__logging_h)																								\
}																															\
while (false)

#define LOGFMT(fmt, ...)	LOG(Logging::string_format(fmt,  ##__VA_ARGS__))

#define HEX(S)			std::hex << std::setfill('0') << std::setw(6) << S
#define HEXN(S, n)		std::hex << std::setfill('0') << std::setw(n) << (uint32_t)S
