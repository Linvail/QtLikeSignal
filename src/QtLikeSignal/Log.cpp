// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! Implementation of the QtLikeSignal::Log namespace: the installed sink, the filter rules, and the
//! per-thread id every record carries.

#include "QtLikeSignal/Log.hpp"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>

#if defined( _WIN32 )
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#else
    #include <sys/syscall.h>
    #include <unistd.h>
#endif

namespace QtLikeSignal
{
    namespace Log
    {
        namespace
        {
            //! Name of the environment variable ensureInitialised() reads.
            const char kRulesVariable[] = "QTLIKESIGNAL_LOG_RULES";

            //! The installed sink, or null while the stderr default is in force.
            //!
            //! Zero-initialised before any dynamic initialisation runs, so a record emitted from a
            //! static constructor finds a valid value rather than one that has not been set up.
            //! That is also why the default is spelled as "null means stderr" rather than as a
            //! pointer initialised to the stderr sink: the latter would depend on the order two
            //! objects in different translation units were constructed in.
            std::atomic<LogSink*> gSink { nullptr };

            //! Guards gRules.
            std::mutex& rulesMutex()
            {
                static std::mutex sMutex;
                return sMutex;
            }

            //! The rule string in force, empty if there is none. Guarded by rulesMutex().
            std::string& rules()
            {
                static std::string sRules;
                return sRules;
            }

            //! Returns whether @p aName matches @p aPattern, which may end in '*'.
            //!
            //! Only a trailing star, and only one. A general glob would let a rule mean things
            //! nobody can predict from reading it, and the thing anyone actually writes is
            //! "everything under this prefix".
            bool matches
                (
                const char* aPattern,      //!< Pattern; not NUL-terminated at aPatternLength.
                std::size_t aPatternLength,
                const char* aName          //!< Category name to test.
                )
            {
                if( aPatternLength > 0 && aPattern[ aPatternLength - 1 ] == '*' )
                {
                    const std::size_t prefix = aPatternLength - 1;
                    return std::strncmp( aPattern, aName, prefix ) == 0;
                }

                const std::size_t nameLength = std::strlen( aName );
                return nameLength == aPatternLength
                       && std::strncmp( aPattern, aName, aPatternLength ) == 0;
            }

            //! Returns the first character of @p aText that is not a space or tab.
            const char* skipSpace
                (
                const char* aText,
                const char* aEnd
                )
            {
                while( aText < aEnd && ( *aText == ' ' || *aText == '\t' ) )
                {
                    ++aText;
                }
                return aText;
            }

            //! Returns the position just past the last character of [@p aBegin, @p aEnd) that is
            //! not a space or a tab.
            const char* trimEnd
                (
                const char* aBegin,
                const char* aEnd
                )
            {
                while( aEnd > aBegin && ( *( aEnd - 1 ) == ' ' || *( aEnd - 1 ) == '\t' ) )
                {
                    --aEnd;
                }
                return aEnd;
            }

            //! Returns the threshold @p aName should have under the current rules.
            //!
            //! **Call with rulesMutex() held.** Split from resolveThreshold() so that
            //! setFilterRules(), which is already holding the lock while it walks the registry,
            //! does not have to take it again per category.
            //!
            //! Every rule is considered rather than stopping at the first match, because later
            //! rules are documented to win: "qtlikesignal.*=warning,qtlikesignal.wayland=debug" has
            //! to mean what it looks like it means.
            LogLevel thresholdForLocked
                (
                const char* aName  //!< Category name.
                )
            {
                LogLevel result = LogCategory::defaultThreshold();

                const std::string& text = rules();
                const char* cursor = text.c_str();
                const char* const end = cursor + text.size();

                while( cursor < end )
                {
                    const char* comma = static_cast<const char*>(
                        std::memchr( cursor, ',', static_cast<std::size_t>( end - cursor ) ) );
                    const char* const entryEnd = comma != nullptr ? comma : end;

                    const char* const equals = static_cast<const char*>(
                        std::memchr( cursor, '=', static_cast<std::size_t>( entryEnd - cursor ) ) );

                    // A rule with no '=' is skipped rather than treated as a pattern with a
                    // missing level: one typo in a long rule string should cost that rule, not
                    // every rule after it.
                    if( equals != nullptr )
                    {
                        // Both ends of both halves are trimmed, so that a rule string can be
                        // written with spaces around the '=' and after the ',' and still mean what
                        // it looks like it means. A pattern with a stray trailing space would
                        // otherwise match nothing at all, silently.
                        const char* const patternBegin = skipSpace( cursor, equals );
                        const char* const patternEnd = trimEnd( patternBegin, equals );
                        const std::size_t patternLength =
                            static_cast<std::size_t>( patternEnd - patternBegin );

                        // The level name has to be NUL-terminated for logLevelFromName(), and the
                        // rule string is not cut up into pieces, so it is copied out. Bounded by
                        // the longest name there is plus room to fail on anything longer.
                        char levelName[ 16 ];
                        const char* const levelBegin = skipSpace( equals + 1, entryEnd );
                        const char* const levelEnd = trimEnd( levelBegin, entryEnd );
                        const std::size_t levelLength =
                            static_cast<std::size_t>( levelEnd - levelBegin );
                        if( levelLength < sizeof( levelName ) )
                        {
                            std::memcpy( levelName, levelBegin, levelLength );
                            levelName[ levelLength ] = '\0';

                            LogLevel level = LogLevel::Info;
                            if( logLevelFromName( levelName, level )
                                && matches( patternBegin, patternLength, aName ) )
                            {
                                result = level;
                            }
                        }
                    }

                    cursor = comma != nullptr ? comma + 1 : end;
                }

                return result;
            }

            //! LogCategory::ThresholdResolver: settles a newly constructed category's threshold.
            LogLevel resolveThreshold
                (
                const LogCategory& aCategory
                )
            {
                std::lock_guard<std::mutex> guard( rulesMutex() );
                return thresholdForLocked( aCategory.name() );
            }

            //! LogCategory::Visitor: re-applies the rules to one already-registered category.
            //!
            //! Called from setFilterRules() with rulesMutex() held, hence the Locked variant.
            void applyRulesTo
                (
                LogCategory& aCategory,
                void*
                )
            {
                aCategory.setThreshold( thresholdForLocked( aCategory.name() ) );
            }
        }

        //! Installs @p aSink as the destination for every record from now on.
        LogSink* setSink
            (
            LogSink* aSink  //!< New sink, or null to go back to the stderr default.
            )
        {
            return gSink.exchange( aSink, std::memory_order_acq_rel );
        }

        //! Returns the installed sink, or the stderr default if none was installed.
        LogSink* sink()
        {
            LogSink* const installed = gSink.load( std::memory_order_acquire );
            return installed != nullptr ? installed : &stderrSink();
        }

        //! Returns the shared sink that writes to stderr.
        //!
        //! A function-local static, so it exists the first time it is asked for and not before.
        //! stderr is unbuffered, so flushing each record costs nothing here.
        LogSinkFile& stderrSink()
        {
            static LogSinkFile sSink( stderr );
            return sSink;
        }

        //! Returns the shared sink that writes to stdout.
        LogSinkFile& stdoutSink()
        {
            static LogSinkFile sSink( stdout );
            return sSink;
        }

        //! Applies @p aRules to every category, now and to every category created later.
        void setFilterRules
            (
            const char* aRules  //!< Rule string; null or empty resets everything to the default.
            )
        {
            ensureInitialised();

            std::lock_guard<std::mutex> guard( rulesMutex() );
            rules().assign( aRules != nullptr ? aRules : "" );

            // Locks in the order rules-then-registry. LogCategory's constructor takes the registry
            // lock, releases it, and only then asks the resolver for the rules -- so it never holds
            // both, and there is no second order for these two locks to be taken in.
            LogCategory::visitAll( &applyRulesTo, nullptr );
        }

        //! Returns the rule string currently in force.
        std::string filterRules()
        {
            std::lock_guard<std::mutex> guard( rulesMutex() );
            return rules();
        }

        //! Reads QTLIKESIGNAL_LOG_RULES and applies it, once per process.
        //!
        //! std::getenv rather than the _s variant, for the reason PlatformIntegration gives: only
        //! MSVC has getenv_s, and its deprecation warning is silenced by the project's own
        //! settings.
        void ensureInitialised()
        {
            static std::once_flag sOnce;
            std::call_once( sOnce, []()
                {
                    LogCategory::setThresholdResolver( &resolveThreshold );

                    const char* const fromEnvironment = std::getenv( kRulesVariable );
                    if( fromEnvironment != nullptr && fromEnvironment[ 0 ] != '\0' )
                    {
                        std::lock_guard<std::mutex> guard( rulesMutex() );
                        rules().assign( fromEnvironment );
                        LogCategory::visitAll( &applyRulesTo, nullptr );
                    }
                } );
        }

        //! Hands one finished record to the installed sink.
        //!
        //! The try/catch is not defensive habit: ~LogRecord may run while an exception is already
        //! propagating, and a second one leaving the destructor would call std::terminate. A sink
        //! that throws has broken its contract, and the record is dropped rather than the process.
        void emitRecord
            (
            const LogMessage& aMessage
            )
        {
            LogSink* const destination = sink();
            if( destination == nullptr )
            {
                return;
            }

            // Swallowed, and compiled out where nothing can throw.
            //
            // A sink is application code called from inside a log statement, which is itself called
            // from anywhere at all -- including a destructor during unwinding, where an escaping
            // exception is std::terminate. Losing one record is the lesser fault.
            //
            // The #if is not decoration here. Applications build this library from source with
            // exceptions disabled, so this file is compiled that way too, and a bare try would be
            // a hard error rather than a dead branch. tools/check-no-exceptions-build.py is what
            // notices; see __cpp_exceptions in MetaCallEvent::create() for why that macro.
            #if defined( __cpp_exceptions )
                try
                {
                    destination->write( aMessage );
                }
                catch( ... )
                {
                }
            #else
                destination->write( aMessage );
            #endif
        }

        //! Pushes anything the installed sink has buffered to its destination.
        void flush()
        {
            LogSink* const destination = sink();
            if( destination == nullptr )
            {
                return;
            }

            // Swallowed and conditionally compiled, for the reason given in emitRecord().
            #if defined( __cpp_exceptions )
                try
                {
                    destination->flush();
                }
                catch( ... )
                {
                }
            #else
                destination->flush();
            #endif
        }

        //! Flushes the installed sink and puts the default back.
        void shutdown()
        {
            flush();
            setSink( nullptr );
        }

        //! Returns the calling thread's id, as the operating system numbers it.
        //!
        //! Cached in a thread_local so the syscall happens once per thread rather than once per
        //! record. On Linux this is gettid(2) through syscall(2) rather than the glibc wrapper,
        //! which only appeared in glibc 2.30 and would put a floor under what this library builds
        //! against for no gain.
        unsigned long long currentThreadId()
        {
            static thread_local unsigned long long sId = 0;
            if( sId == 0 )
            {
                #if defined( _WIN32 )
                    sId = static_cast<unsigned long long>( ::GetCurrentThreadId() );
                #else
                    sId = static_cast<unsigned long long>( ::syscall( SYS_gettid ) );
                #endif
            }
            return sId;
        }
    }
}
