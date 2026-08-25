// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! Turning what Linux calls a key into what this library calls one.
//!
//! Shared by the Wayland and DRM backends, which is the whole reason it is a file rather than two
//! copies of a switch: wl_keyboard passes the kernel's evdev codes through unchanged and libinput
//! reports the same ones, so both backends are answering an identical question. The Win32 and X11
//! backends translate from their own platforms' codes and keep their tables to themselves.

#ifndef QT_LIKE_SIGNAL_GUI_KEYTRANSLATION_HPP
#define QT_LIKE_SIGNAL_GUI_KEYTRANSLATION_HPP

#include "QtLikeSignalGui/InputEvents.hpp"

#include <linux/input-event-codes.h>

namespace QtLikeSignalGui
{
    //! Translates a Linux evdev key code into a portable Key.
    //!
    //! **Positional, not layout-aware, and that is the correct answer here.** An evdev code names a
    //! place on the keyboard, and so does Key -- KEY_Q is the key where Q sits on a US layout, and
    //! this reports Key::Q for it whatever the user's layout prints on the cap. Working out which
    //! *character* that key produces is a different question, needs the keymap, and is answered by
    //! KeyEvent::mText, which these two backends leave empty. See the backends for why.
    //!
    //! @return the portable key, or Key::Unknown for anything not named here.
    inline Key keyFromEvdevCode
        (
        unsigned int aCode   //!< The evdev code, as wl_keyboard or libinput reports it.
        )
    {
        switch( aCode )
        {
        case KEY_A: return Key::A;
        case KEY_B: return Key::B;
        case KEY_C: return Key::C;
        case KEY_D: return Key::D;
        case KEY_E: return Key::E;
        case KEY_F: return Key::F;
        case KEY_G: return Key::G;
        case KEY_H: return Key::H;
        case KEY_I: return Key::I;
        case KEY_J: return Key::J;
        case KEY_K: return Key::K;
        case KEY_L: return Key::L;
        case KEY_M: return Key::M;
        case KEY_N: return Key::N;
        case KEY_O: return Key::O;
        case KEY_P: return Key::P;
        case KEY_Q: return Key::Q;
        case KEY_R: return Key::R;
        case KEY_S: return Key::S;
        case KEY_T: return Key::T;
        case KEY_U: return Key::U;
        case KEY_V: return Key::V;
        case KEY_W: return Key::W;
        case KEY_X: return Key::X;
        case KEY_Y: return Key::Y;
        case KEY_Z: return Key::Z;

        case KEY_0: return Key::Digit0;
        case KEY_1: return Key::Digit1;
        case KEY_2: return Key::Digit2;
        case KEY_3: return Key::Digit3;
        case KEY_4: return Key::Digit4;
        case KEY_5: return Key::Digit5;
        case KEY_6: return Key::Digit6;
        case KEY_7: return Key::Digit7;
        case KEY_8: return Key::Digit8;
        case KEY_9: return Key::Digit9;

        case KEY_SPACE:      return Key::Space;
        case KEY_APOSTROPHE: return Key::Apostrophe;
        case KEY_COMMA:      return Key::Comma;
        case KEY_MINUS:      return Key::Minus;
        case KEY_DOT:        return Key::Period;
        case KEY_SLASH:      return Key::Slash;
        case KEY_SEMICOLON:  return Key::Semicolon;
        case KEY_EQUAL:      return Key::Equal;
        case KEY_LEFTBRACE:  return Key::BracketLeft;
        case KEY_BACKSLASH:  return Key::Backslash;
        case KEY_RIGHTBRACE: return Key::BracketRight;
        case KEY_GRAVE:      return Key::Grave;

        case KEY_ESC:       return Key::Escape;
        case KEY_TAB:       return Key::Tab;
        case KEY_BACKSPACE: return Key::Backspace;
        case KEY_ENTER:     return Key::Return;
        case KEY_KPENTER:   return Key::Enter;
        case KEY_INSERT:    return Key::Insert;
        case KEY_DELETE:    return Key::Delete;
        case KEY_PAUSE:     return Key::Pause;
        case KEY_SYSRQ:     return Key::Print;

        case KEY_HOME:     return Key::Home;
        case KEY_END:      return Key::End;
        case KEY_LEFT:     return Key::Left;
        case KEY_UP:       return Key::Up;
        case KEY_RIGHT:    return Key::Right;
        case KEY_DOWN:     return Key::Down;
        case KEY_PAGEUP:   return Key::PageUp;
        case KEY_PAGEDOWN: return Key::PageDown;

        case KEY_LEFTSHIFT:
        case KEY_RIGHTSHIFT: return Key::Shift;
        case KEY_LEFTCTRL:
        case KEY_RIGHTCTRL:  return Key::Control;
        case KEY_LEFTALT:
        case KEY_RIGHTALT:   return Key::Alt;
        case KEY_LEFTMETA:
        case KEY_RIGHTMETA:  return Key::Meta;
        case KEY_CAPSLOCK:   return Key::CapsLock;
        case KEY_NUMLOCK:    return Key::NumLock;
        case KEY_SCROLLLOCK: return Key::ScrollLock;
        case KEY_COMPOSE:    return Key::Menu;

        case KEY_F1:  return Key::F1;
        case KEY_F2:  return Key::F2;
        case KEY_F3:  return Key::F3;
        case KEY_F4:  return Key::F4;
        case KEY_F5:  return Key::F5;
        case KEY_F6:  return Key::F6;
        case KEY_F7:  return Key::F7;
        case KEY_F8:  return Key::F8;
        case KEY_F9:  return Key::F9;
        case KEY_F10: return Key::F10;
        case KEY_F11: return Key::F11;
        case KEY_F12: return Key::F12;

        default: break;
        }

        return Key::Unknown;
    }

    //! Returns the modifier a key *is*, or KeyModifier::None for an ordinary key.
    //!
    //! **This is how the two Linux backends know their modifier state at all.** Neither takes a
    //! dependency on xkbcommon, so neither can interpret the modifier masks its platform sends --
    //! those are indices into a keymap this library never parses. Watching the modifier keys
    //! themselves go down and up gives the same answer for every modifier that matters, without
    //! the dependency.
    //!
    //! The cost is that a modifier pressed while the window did not have focus is not seen, so the
    //! set can be wrong until that key is next released. The backends resynchronise on focus, which
    //! is the only moment the discrepancy can arise.
    inline KeyModifier modifierForKey
        (
        Key aKey   //!< The key that changed.
        )
    {
        switch( aKey )
        {
        case Key::Shift:   return KeyModifier::Shift;
        case Key::Control: return KeyModifier::Control;
        case Key::Alt:     return KeyModifier::Alt;
        case Key::Meta:    return KeyModifier::Meta;
        default:           return KeyModifier::None;
        }
    }
}

#endif // QT_LIKE_SIGNAL_GUI_KEYTRANSLATION_HPP
