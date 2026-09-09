// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! Helpers shared by the QtLikeSignalGui tests.

#ifndef QT_LIKE_SIGNAL_GUI_TEST_SUPPORT_HPP
#define QT_LIKE_SIGNAL_GUI_TEST_SUPPORT_HPP

#include <string>
#include <vector>

namespace QtLikeSignalGuiTest
{
    //! An argv, built the way main() receives one, so a test can drive platform selection.
    //!
    //! GuiApplication takes the arguments as `char**` and copies them during construction, so the
    //! storage only has to outlive the constructor -- but it does have to be writable, which a
    //! string literal is not. This owns it.
    class FakeCommandLine
    {
    public:
        //! Constructs a command line: argv[0] is "test", followed by @p aArguments.
        explicit FakeCommandLine
            (
            const std::vector<std::string>& aArguments   //!< Arguments after argv[0].
            )
        {
            mStorage.push_back( "test" );
            for( const std::string& argument : aArguments )
            {
                mStorage.push_back( argument );
            }

            // Filled after mStorage is complete, never during: every push_back before this point
            // may reallocate, and a pointer taken beforehand would be into the freed buffer.
            mPointers.reserve( mStorage.size() );
            for( std::string& argument : mStorage )
            {
                mPointers.push_back( &argument[0] );
            }
        }

        //! Gets argc.
        int argc() const
        {
            return static_cast<int>( mPointers.size() );
        }

        //! Gets argv.
        char** argv()
        {
            return mPointers.data();
        }

    private:
        std::vector<std::string> mStorage;   //!< Owns the characters argv points at.
        std::vector<char*> mPointers;        //!< The argv array itself.
    };
}

#endif // QT_LIKE_SIGNAL_GUI_TEST_SUPPORT_HPP
