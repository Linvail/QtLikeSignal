// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! GoogleTest suite for the logging facility: LogLevel, LogCategory, LogRecord, the stream
//! operators, the filter rules, and the sink installed under them.

#include "QtLikeSignal/Log.hpp"

#include "gtest/gtest.h"
#include <atomic>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
    using namespace QtLikeSignal;

    //! A sink that keeps what it was handed, so a test can assert on it.
    //!
    //! It copies the text, which LogMessage documents as the obligation of any sink that outlives
    //! the call. That is not incidental to the test: a capture sink that stored the pointer would
    //! read the caller's dead stack frame, and getting it right here is the worked example of the
    //! rule for anyone writing a real sink.
    class CaptureSink : public LogSink
    {
    public:
        //! One captured record, with everything the test might want to assert on.
        struct Entry
        {
            LogLevel mLevel;             //!< Severity it came in at.
            std::string mCategoryName;   //!< Name of the category it came from.
            std::string mText;           //!< The formatted text.
            std::string mFile;           //!< Source file it came from.
            int mLine;                   //!< Source line.
            unsigned long long mThreadId;//!< Thread that logged it.
            bool mTruncated;             //!< Whether it was cut short.
        };

        //! Copies @p aMessage into the captured list.
        virtual void write
            (
            const LogMessage& aMessage
            ) override
        {
            Entry entry;
            entry.mLevel = aMessage.mLevel;
            entry.mCategoryName =
                aMessage.mCategory != nullptr ? aMessage.mCategory->name() : std::string();
            entry.mText = std::string( aMessage.mText, aMessage.mLength );
            entry.mFile = aMessage.mFile != nullptr ? aMessage.mFile : "";
            entry.mLine = aMessage.mLine;
            entry.mThreadId = aMessage.mThreadId;
            entry.mTruncated = aMessage.mTruncated;

            std::lock_guard<std::mutex> guard( mMutex );
            mEntries.push_back( entry );
        }

        //! Counts a flush, so a test can tell Log::flush() reached the sink.
        virtual void flush() override
        {
            mFlushCount.fetch_add( 1 );
        }

        //! @return a copy of everything captured so far.
        std::vector<Entry> entries() const
        {
            std::lock_guard<std::mutex> guard( mMutex );
            return mEntries;
        }

        //! @return how many records have been captured.
        std::size_t count() const
        {
            std::lock_guard<std::mutex> guard( mMutex );
            return mEntries.size();
        }

        //! @return the most recently captured record. The caller must have checked count() first.
        Entry last() const
        {
            std::lock_guard<std::mutex> guard( mMutex );
            return mEntries.back();
        }

        //! Discards everything captured so far.
        void clear()
        {
            std::lock_guard<std::mutex> guard( mMutex );
            mEntries.clear();
        }

        //! @return how many times flush() has been called.
        int flushCount() const
        {
            return mFlushCount.load();
        }

    private:
        mutable std::mutex mMutex;      //!< Guards mEntries; write() is called from many threads.
        std::vector<Entry> mEntries;    //!< Everything captured, oldest first.
        std::atomic<int> mFlushCount { 0 };  //!< How many times flush() has been called.
    };

    //! Categories used only by this suite.
    //!
    //! Named under "qtlikesignal.test." so that a filter rule aimed at them cannot accidentally
    //! match a category the library itself uses, and vice versa -- the registry is process-wide and
    //! every test in this binary shares it.
    //! @{
    QTLIKESIGNAL_DEFINE_LOG_CATEGORY( gAlpha, "qtlikesignal.test.alpha", "TALP" )
    QTLIKESIGNAL_DEFINE_LOG_CATEGORY( gBeta, "qtlikesignal.test.beta", "TBET" )
    //! @}

    //! Counts how often it has been called, so a test can prove an argument was never evaluated.
    int gEvaluationCount = 0;

    //! Increments gEvaluationCount and returns something streamable.
    int countEvaluation()
    {
        ++gEvaluationCount;
        return gEvaluationCount;
    }

    //! Installs a capture sink and known filter rules, and puts both back afterwards.
    class LoggingTest : public ::testing::Test
    {
    protected:
        //! Installs the capture sink, and switches both test categories fully on.
        virtual void SetUp() override
        {
            mPrevious = Log::setSink( &mSink );
            Log::setFilterRules( "qtlikesignal.test.*=debug" );
            gEvaluationCount = 0;
        }

        //! Restores the sink and clears the rules.
        //!
        //! Both matter to the tests that run after this one in the same binary: the sink is
        //! process-wide, and a rule left in place would follow the suite out of this file.
        virtual void TearDown() override
        {
            Log::setFilterRules( "" );
            Log::setSink( mPrevious );
        }

        CaptureSink mSink;               //!< The sink under test.
        LogSink* mPrevious { nullptr };  //!< Whatever was installed before, restored in TearDown.
    };

    //! logLevelName() and logLevelFromName() agree in both directions, for every level.
    TEST( LogLevelTest, NamesRoundTrip )
    {
        const LogLevel levels[] =
        {
            LogLevel::Debug, LogLevel::Info, LogLevel::Warning, LogLevel::Critical
        };

        for( LogLevel level : levels )
        {
            LogLevel parsed = LogLevel::Critical;
            ASSERT_TRUE( logLevelFromName( logLevelName( level ), parsed ) )
                << "failed to parse " << logLevelName( level );
            EXPECT_EQ( level, parsed );
        }
    }

    //! A name that is not one of the four is rejected, and leaves the out parameter alone.
    TEST( LogLevelTest, RejectsUnknownNames )
    {
        LogLevel level = LogLevel::Warning;
        EXPECT_FALSE( logLevelFromName( "verbose", level ) );
        EXPECT_FALSE( logLevelFromName( "Warning", level ) );  // Case-sensitive, deliberately.
        EXPECT_FALSE( logLevelFromName( "", level ) );
        EXPECT_FALSE( logLevelFromName( nullptr, level ) );
        EXPECT_EQ( LogLevel::Warning, level );
    }

    //! A category reports the name and short id it was defined with.
    TEST( LogCategoryTest, CarriesBothNames )
    {
        EXPECT_STREQ( "qtlikesignal.test.alpha", gAlpha().name() );
        EXPECT_STREQ( "TALP", gAlpha().shortId() );
    }

    //! A short id longer than a DLT context id is truncated rather than overrunning the buffer.
    TEST( LogCategoryTest, TruncatesOverlongShortId )
    {
        LogCategory category( "qtlikesignal.test.overlong", "TOOLONG" );
        EXPECT_STREQ( "TOOL", category.shortId() );
        EXPECT_EQ( 4u, std::string( category.shortId() ).size() );
    }

    //! With no rules in force, Debug is off and everything else is on.
    TEST( LogCategoryTest, DefaultThresholdSuppressesDebugOnly )
    {
        LogCategory category( "qtlikesignal.test.defaults", "TDEF" );
        EXPECT_EQ( LogCategory::defaultThreshold(), category.threshold() );
        EXPECT_FALSE( category.isEnabled( LogLevel::Debug ) );
        EXPECT_TRUE( category.isEnabled( LogLevel::Info ) );
        EXPECT_TRUE( category.isEnabled( LogLevel::Warning ) );
        EXPECT_TRUE( category.isEnabled( LogLevel::Critical ) );
    }

    //! A record reaches the installed sink carrying its level, category and source location.
    TEST_F( LoggingTest, RecordCarriesItsProvenance )
    {
        const int expectedLine = __LINE__ + 1;
        qCWarning( gAlpha ) << "hello";

        ASSERT_EQ( 1u, mSink.count() );
        const CaptureSink::Entry entry = mSink.last();
        EXPECT_EQ( LogLevel::Warning, entry.mLevel );
        EXPECT_EQ( "qtlikesignal.test.alpha", entry.mCategoryName );
        EXPECT_EQ( "hello", entry.mText );
        EXPECT_EQ( expectedLine, entry.mLine );
        EXPECT_NE( std::string::npos, entry.mFile.find( "QtLikeSignal-test-logging" ) );
        EXPECT_EQ( Log::currentThreadId(), entry.mThreadId );
        EXPECT_FALSE( entry.mTruncated );
    }

    //! Values are separated by a single space, and nothing is put in front of the first.
    TEST_F( LoggingTest, SeparatesValuesWithOneSpace )
    {
        qCInfo( gAlpha ) << "interval" << 250 << "ms";
        ASSERT_EQ( 1u, mSink.count() );
        EXPECT_EQ( "interval 250 ms", mSink.last().mText );
    }

    //! A record with nothing streamed into it is still a valid, empty record.
    TEST_F( LoggingTest, EmptyRecordIsEmptyNotNull )
    {
        qCInfo( gAlpha );
        ASSERT_EQ( 1u, mSink.count() );
        EXPECT_EQ( "", mSink.last().mText );
    }

    //! Every stream operator renders its type the way the header says it does.
    TEST_F( LoggingTest, StreamOperatorsRenderEveryType )
    {
        qCInfo( gAlpha ) << true << false;
        EXPECT_EQ( "true false", mSink.last().mText );

        qCInfo( gAlpha ) << 'x';
        EXPECT_EQ( "x", mSink.last().mText );

        qCInfo( gAlpha ) << 0 << -1 << 42;
        EXPECT_EQ( "0 -1 42", mSink.last().mText );

        qCInfo( gAlpha ) << static_cast<short>( -7 )
                         << static_cast<unsigned short>( 7 );
        EXPECT_EQ( "-7 7", mSink.last().mText );

        qCInfo( gAlpha ) << 4000000000u << 123456789012345LL;
        EXPECT_EQ( "4000000000 123456789012345", mSink.last().mText );

        // The most negative value there is. Its magnitude does not fit in the signed type, which
        // is exactly the case a hand-rolled integer formatter gets wrong.
        qCInfo( gAlpha ) << ( -9223372036854775807LL - 1 );
        EXPECT_EQ( "-9223372036854775808", mSink.last().mText );

        qCInfo( gAlpha ) << 18446744073709551615ULL;
        EXPECT_EQ( "18446744073709551615", mSink.last().mText );

        qCInfo( gAlpha ) << 1.5 << 0.25f;
        EXPECT_EQ( "1.5 0.25", mSink.last().mText );

        qCInfo( gAlpha ) << std::string( "std" ) << "literal";
        EXPECT_EQ( "std literal", mSink.last().mText );

        qCInfo( gAlpha ) << std::string_view( "view" );
        EXPECT_EQ( "view", mSink.last().mText );

        qCInfo( gAlpha ) << static_cast<const char*>( nullptr );
        EXPECT_EQ( "(null)", mSink.last().mText );

        qCInfo( gAlpha ) << nullptr;
        EXPECT_EQ( "nullptr", mSink.last().mText );

        qCInfo( gAlpha ) << static_cast<const void*>( nullptr );
        EXPECT_EQ( "nullptr", mSink.last().mText );
    }

    //! A non-null pointer prints as fixed-width lower-case hex with an 0x prefix.
    TEST_F( LoggingTest, RendersPointersAsFixedWidthHex )
    {
        int value = 0;
        qCInfo( gAlpha ) << static_cast<const void*>( &value );

        const std::string text = mSink.last().mText;
        ASSERT_EQ( 2u + ( sizeof( void* ) * 2 ), text.size() );
        EXPECT_EQ( "0x", text.substr( 0, 2 ) );
        EXPECT_EQ( std::string::npos, text.find_first_not_of( "0123456789abcdef", 2 ) );
    }

    //! logHex() renders an integer in hexadecimal, padded to the width it was asked for.
    //!
    //! The widths matter as much as the digits: an EGL error is quoted everywhere as four digits,
    //! and a value that prints as 0x9 where the documentation says 0x0009 is one a reader has to
    //! stop and think about.
    TEST_F( LoggingTest, RendersIntegersAsHexOnRequest )
    {
        qCInfo( gAlpha ) << logHex( 0x3002u, 4 );
        EXPECT_EQ( "0x3002", mSink.last().mText );

        qCInfo( gAlpha ) << logHex( 0x18u );
        EXPECT_EQ( "0x18", mSink.last().mText );

        qCInfo( gAlpha ) << logHex( 0u, 4 );
        EXPECT_EQ( "0x0000", mSink.last().mText );

        qCInfo( gAlpha ) << logHex( 0u );
        EXPECT_EQ( "0x0", mSink.last().mText );

        // Wider than the request, which must not be truncated to it.
        qCInfo( gAlpha ) << logHex( 0x3c00003u, 4 );
        EXPECT_EQ( "0x3c00003", mSink.last().mText );

        qCInfo( gAlpha ) << logHex( 0xFFFFFFFFFFFFFFFFULL );
        EXPECT_EQ( "0xffffffffffffffff", mSink.last().mText );

        // A minimum past the sixteen digits a 64-bit value has cannot add information, and must
        // not walk off the front of the record's conversion buffer.
        qCInfo( gAlpha ) << logHex( 1u, 99 );
        EXPECT_EQ( "0x0000000000000001", mSink.last().mText );
    }

    //! An object pointer with no overload of its own still prints, through the const void* one.
    TEST_F( LoggingTest, RendersArbitraryObjectPointers )
    {
        struct Unknown
        {
            int mValue;  //!< Present only so the type is not empty.
        };

        Unknown unknown { 0 };
        qCInfo( gAlpha ) << &unknown;
        EXPECT_EQ( "0x", mSink.last().mText.substr( 0, 2 ) );
    }

    //! A category below its threshold emits nothing.
    TEST_F( LoggingTest, SuppressesBelowThreshold )
    {
        Log::setFilterRules( "qtlikesignal.test.*=warning" );

        qCDebug( gAlpha ) << "debug";
        qCInfo( gAlpha ) << "info";
        EXPECT_EQ( 0u, mSink.count() );

        qCWarning( gAlpha ) << "warning";
        qCCritical( gAlpha ) << "critical";
        EXPECT_EQ( 2u, mSink.count() );
    }

    //! **The property the macro exists for.** A suppressed record evaluates none of its arguments.
    //!
    //! This is what separates the macro from a variadic function, which would have to evaluate
    //! every argument before it could look at the category. Break it and nothing fails except the
    //! frame budget of whoever left a `<< expensiveDump()` in a render loop.
    TEST_F( LoggingTest, SuppressedRecordEvaluatesNothing )
    {
        Log::setFilterRules( "qtlikesignal.test.*=critical" );

        qCDebug( gAlpha ) << countEvaluation();
        qCInfo( gAlpha ) << countEvaluation() << countEvaluation();
        qCWarning( gAlpha ) << countEvaluation();
        EXPECT_EQ( 0, gEvaluationCount );
        EXPECT_EQ( 0u, mSink.count() );

        qCCritical( gAlpha ) << countEvaluation();
        EXPECT_EQ( 1, gEvaluationCount );
        EXPECT_EQ( 1u, mSink.count() );
    }

    //! qWarning() and its three siblings report on a category named "default".
    //!
    //! The name matters as much as the routing: Qt calls its own uncategorized category "default"
    //! too, so a rule written for one toolkit means the same thing in the other.
    TEST_F( LoggingTest, UncategorizedMacrosReportOnTheDefaultCategory )
    {
        qWarning() << "uncategorized";

        ASSERT_EQ( 1u, mSink.count() );
        EXPECT_EQ( "default", mSink.entries()[ 0 ].mCategoryName );
        EXPECT_EQ( "uncategorized", mSink.entries()[ 0 ].mText );
    }

    //! Each of the four uncategorized macros carries the level its name says.
    //!
    //! Debug is switched on first, because "default" is not matched by the fixture's rule and
    //! would otherwise be at the default threshold, where Debug is off.
    TEST_F( LoggingTest, UncategorizedMacrosCarryTheirLevel )
    {
        Log::setFilterRules( "default=debug" );

        qDebug() << "d";
        qInfo() << "i";
        qWarning() << "w";
        qCritical() << "c";

        ASSERT_EQ( 4u, mSink.count() );
        EXPECT_EQ( LogLevel::Debug, mSink.entries()[ 0 ].mLevel );
        EXPECT_EQ( LogLevel::Info, mSink.entries()[ 1 ].mLevel );
        EXPECT_EQ( LogLevel::Warning, mSink.entries()[ 2 ].mLevel );
        EXPECT_EQ( LogLevel::Critical, mSink.entries()[ 3 ].mLevel );
    }

    //! A rule aimed at the library does not reach the caller's own uncategorized records.
    //!
    //! This is the whole reason "default" is not spelled "qtlikesignal.default". An application
    //! that silences this library must not silence itself as a side effect.
    TEST_F( LoggingTest, DefaultCategoryIsOutsideTheLibraryNamespace )
    {
        Log::setFilterRules( "qtlikesignal.*=critical" );

        qWarning() << "still reported";

        ASSERT_EQ( 1u, mSink.count() );
        EXPECT_EQ( "default", mSink.entries()[ 0 ].mCategoryName );
    }

    //! A suppressed uncategorized record evaluates nothing either.
    //!
    //! The categorized macros are covered above. These expand through them, so this asserts the
    //! expansion did not lose the property rather than re-testing it.
    TEST_F( LoggingTest, SuppressedUncategorizedRecordEvaluatesNothing )
    {
        Log::setFilterRules( "default=critical" );

        qDebug() << countEvaluation();
        qInfo() << countEvaluation();
        qWarning() << countEvaluation();
        EXPECT_EQ( 0, gEvaluationCount );
        EXPECT_EQ( 0u, mSink.count() );

        qCritical() << countEvaluation();
        EXPECT_EQ( 1, gEvaluationCount );
        EXPECT_EQ( 1u, mSink.count() );
    }

    //! A log statement in front of a caller's own else binds the way the caller wrote it.
    //!
    //! The macro expands to an if/else, so a naive spelling would steal the caller's else. This
    //! test is here to fail at compile time if the spelling ever changes.
    TEST_F( LoggingTest, DoesNotStealADanglingElse )
    {
        bool tookElse = false;

        if( mSink.count() != 0u )
            qCInfo( gAlpha ) << "never";
        else
            tookElse = true;

        EXPECT_TRUE( tookElse );
        EXPECT_EQ( 0u, mSink.count() );
    }

    //! Later rules win over earlier ones, so a wildcard can be qualified by a specific name.
    TEST_F( LoggingTest, LaterRulesWin )
    {
        Log::setFilterRules( "qtlikesignal.test.*=warning,qtlikesignal.test.beta=debug" );

        qCDebug( gAlpha ) << "alpha";
        qCDebug( gBeta ) << "beta";

        ASSERT_EQ( 1u, mSink.count() );
        EXPECT_EQ( "qtlikesignal.test.beta", mSink.last().mCategoryName );
    }

    //! An exact rule matches only that category, not the ones whose names start the same way.
    TEST_F( LoggingTest, ExactRulesDoNotMatchByPrefix )
    {
        Log::setFilterRules( "qtlikesignal.test.alph=debug" );

        qCDebug( gAlpha ) << "alpha";
        EXPECT_EQ( 0u, mSink.count() );
    }

    //! A malformed entry is skipped on its own; the entries around it still apply.
    TEST_F( LoggingTest, MalformedRulesDoNotDiscardTheRest )
    {
        Log::setFilterRules(
            "nonsense,qtlikesignal.test.*=notalevel,qtlikesignal.test.alpha=debug" );

        qCDebug( gAlpha ) << "alpha";
        qCDebug( gBeta ) << "beta";

        ASSERT_EQ( 1u, mSink.count() );
        EXPECT_EQ( "qtlikesignal.test.alpha", mSink.last().mCategoryName );
    }

    //! Whitespace around a pattern and a level is ignored, so a rule string can be readable.
    TEST_F( LoggingTest, ToleratesWhitespaceInRules )
    {
        Log::setFilterRules( " qtlikesignal.test.alpha = debug " );

        qCDebug( gAlpha ) << "alpha";
        EXPECT_EQ( 1u, mSink.count() );
    }

    //! Clearing the rules puts every category back to the default rather than leaving the last one.
    TEST_F( LoggingTest, ClearingRulesRestoresTheDefault )
    {
        Log::setFilterRules( "qtlikesignal.test.*=debug" );
        EXPECT_TRUE( gAlpha().isEnabled( LogLevel::Debug ) );

        Log::setFilterRules( "" );
        EXPECT_FALSE( gAlpha().isEnabled( LogLevel::Debug ) );
        EXPECT_TRUE( gAlpha().isEnabled( LogLevel::Info ) );
    }

    //! A category created after the rules were set still picks them up.
    //!
    //! Categories are function-local statics, so one can come into existence at any point in a
    //! run. Without the resolver hook this is the case that silently ignores its rule.
    TEST_F( LoggingTest, RulesReachCategoriesCreatedLater )
    {
        Log::setFilterRules( "qtlikesignal.test.*=critical" );

        LogCategory late( "qtlikesignal.test.late", "TLAT" );
        EXPECT_EQ( LogLevel::Critical, late.threshold() );
        EXPECT_FALSE( late.isEnabled( LogLevel::Warning ) );
    }

    //! filterRules() reports back what was set.
    TEST_F( LoggingTest, ReportsTheRulesInForce )
    {
        Log::setFilterRules( "qtlikesignal.test.alpha=info" );
        EXPECT_EQ( "qtlikesignal.test.alpha=info", Log::filterRules() );
    }

    //! Text past the record's capacity is cut short, marked, and ends in an ellipsis.
    TEST_F( LoggingTest, TruncatesOverlongText )
    {
        const std::string huge( LogRecord::kCapacity * 2, 'x' );
        qCInfo( gAlpha ) << huge;

        ASSERT_EQ( 1u, mSink.count() );
        const CaptureSink::Entry entry = mSink.last();
        EXPECT_TRUE( entry.mTruncated );
        EXPECT_EQ( LogRecord::kTextCapacity + 3, entry.mText.size() );
        EXPECT_EQ( "...", entry.mText.substr( entry.mText.size() - 3 ) );
        EXPECT_LT( entry.mText.size(), LogRecord::kCapacity );
    }

    //! Once truncated, a record accepts nothing further and grows exactly one ellipsis.
    TEST_F( LoggingTest, TruncatedRecordStopsAccepting )
    {
        const std::string huge( LogRecord::kCapacity * 2, 'x' );
        qCInfo( gAlpha ) << huge << "tail" << 1 << 2;

        const std::string text = mSink.last().mText;
        EXPECT_EQ( std::string::npos, text.find( "tail" ) );
        EXPECT_EQ( LogRecord::kTextCapacity + 3, text.size() );
    }

    //! Text that exactly fills the capacity is not marked truncated.
    TEST_F( LoggingTest, ExactFitIsNotTruncated )
    {
        const std::string exact( LogRecord::kTextCapacity, 'y' );
        qCInfo( gAlpha ) << exact;

        const CaptureSink::Entry entry = mSink.last();
        EXPECT_FALSE( entry.mTruncated );
        EXPECT_EQ( LogRecord::kTextCapacity, entry.mText.size() );
    }

    //! setSink() hands back what was installed before, so a caller can put it back.
    TEST_F( LoggingTest, SetSinkReturnsThePrevious )
    {
        CaptureSink other;
        LogSink* const previous = Log::setSink( &other );
        EXPECT_EQ( &mSink, previous );

        qCInfo( gAlpha ) << "elsewhere";
        EXPECT_EQ( 1u, other.count() );
        EXPECT_EQ( 0u, mSink.count() );

        Log::setSink( previous );
        qCInfo( gAlpha ) << "back";
        EXPECT_EQ( 1u, mSink.count() );
    }

    //! Log::flush() reaches the installed sink.
    TEST_F( LoggingTest, FlushReachesTheSink )
    {
        EXPECT_EQ( 0, mSink.flushCount() );
        Log::flush();
        EXPECT_EQ( 1, mSink.flushCount() );
    }

    //! A sink that throws does not let the exception out of ~LogRecord.
    //!
    //! A destructor that lets one escape while another is already propagating calls
    //! std::terminate, so this is the difference between a broken sink and a dead process.
    TEST_F( LoggingTest, SwallowsAnExceptionFromTheSink )
    {
        //! A deliberately broken sink, for the one thing that must survive it.
        class ThrowingSink : public LogSink
        {
        public:
            virtual void write
                (
                const LogMessage&
                ) override
            {
                throw std::runtime_error( "sink" );
            }

        };

        ThrowingSink throwing;
        LogSink* const previous = Log::setSink( &throwing );
        EXPECT_NO_THROW( qCWarning( gAlpha ) << "boom" );
        Log::setSink( previous );
    }

    //! Records from many threads at once all arrive, each one whole.
    //!
    //! The text is built in the record's own buffer, which is on the logging thread's stack, so
    //! nothing is shared until the sink is called. What this checks is that the claim holds: every
    //! record arrives, and none of them is a mixture of two threads' text.
    TEST_F( LoggingTest, IsSafeUnderConcurrentLogging )
    {
        const int threadCount = 8;
        const int perThread = 200;

        std::vector<std::thread> threads;
        threads.reserve( threadCount );
        for( int index = 0; index < threadCount; ++index )
        {
            threads.emplace_back( [index, perThread]()
                {
                    for( int record = 0; record < perThread; ++record )
                    {
                        qCInfo( gAlpha ) << "thread" << index << "record" << record;
                    }
                } );
        }

        for( std::thread& thread : threads )
        {
            thread.join();
        }

        const std::vector<CaptureSink::Entry> entries = mSink.entries();
        ASSERT_EQ( static_cast<std::size_t>( threadCount * perThread ), entries.size() );

        for( const CaptureSink::Entry& entry : entries )
        {
            EXPECT_EQ( "thread", entry.mText.substr( 0, 6 ) )
                << "record was not whole: " << entry.mText;
            EXPECT_FALSE( entry.mTruncated );
        }
    }
}
