// SPDX-FileCopyrightText: 2026 Evan
// SPDX-License-Identifier: MIT

//! @file
//!
//! QtLikeSignal::Timer - repeating and single-shot timers, mirroring Qt's QTimer.
//!
//! A Timer is an Object, so it lives in a thread; its timeout signal is emitted by that thread's
//! event loop. The loop is what drives it, so a Timer belonging to a thread that never runs one
//! (a bare adopted thread, say) will never fire.

#ifndef QT_LIKE_SIGNAL_TIMER_HPP
#define QT_LIKE_SIGNAL_TIMER_HPP

#include "QtLikeSignal/Event.hpp"
#include "QtLikeSignal/Object.hpp"
#include "QtLikeSignal/Signal.hpp"

#include <utility>

namespace QtLikeSignal
{
    //! High-level timer class providing repetitive and single-shot timers.
    //!
    //! **No part of this class is thread-safe**, exactly as with Qt's QTimer. A Timer must be used
    //! from the thread it lives in: start()/stop() are thread-confined because they go through
    //! Object::startTimer()/killTimer(), and timeout is emitted by that thread's event loop. To drive
    //! a timer that lives in another thread, hop onto it first, e.g.
    //! `Object::callLater(&timer, &Timer::start, 50)`.
    class Timer : public Object
    {
    public:
        explicit Timer
            (
            Thread* aThread = nullptr
            );

        virtual ~Timer() override;

        int interval() const;

        void setInterval
            (
            int aMsec
            );

        bool isActive() const;

        bool isSingleShot() const;

        void setSingleShot
            (
            bool aSingleShot
            );

        int timerId() const;

        //! Starts or restarts the timer with specified interval in milliseconds.
        //!
        //! **Must be called from this timer's own thread**, because it goes through
        //! Object::startTimer(); see that function for why. Same rule as Qt's QTimer, whose start()
        //! is likewise a plain forward to QObject::startTimer(). To start a timer that lives in
        //! another thread, hop onto that thread first, e.g.
        //! `Object::callLater(&timer, &Timer::start, 50)`.
        void start
            (
            int aMsec  //!< Interval in milliseconds.
            );

        void start();

        void stop();

        SignalView<>& getTimeout() const;

        //! Fires a single-shot timer executing a functor after specified delay. Functor is the
        //! callable slot type.
        template <typename Functor> static void singleShot
            (
            int aMsec,
            Functor aFunctor
            );

        //! Fires a single-shot timer executing a functor in context object's thread. Functor is
        //! the callable slot type.
        template <typename Functor> static void singleShot
            (
            int aMsec,
            const Object* aContext,
            Functor aFunctor
            );

        //! Fires a single-shot timer executing a member function on receiver object. Receiver is
        //! the receiver object type and MemberFunc the member function pointer type.
        template <typename Receiver, typename MemberFunc> static void singleShot
            (
            int aMsec,
            const Receiver* aReceiver,
            MemberFunc aMethod
            );

    protected:
        virtual void timerEvent
            (
            TimerEvent* aEvent
            ) override;

    private:
        //! Corrects a caller-supplied interval to one a timer can actually run at.
        //!
        //! Mirrors Qt's checkInterval() in qtimer.cpp, including its choice to correct rather than
        //! reject: Qt treats a negative interval as a caller mistake worth reporting but not worth
        //! cancelling the call over, so the timer still runs, just at the shortest interval there
        //! is. Shared by every entry point that takes one so none of them can drift from the
        //! others. Returns the interval to use, in milliseconds.
        static int checkInterval
            (
            const char* aCaller,  //!< Name of the calling function, for the warning text.
            int aMsec             //!< The requested interval, in milliseconds.
            );

        // Deliberately unsynchronised, matching QTimer, which has no locking of any kind. Every
        // member here is only ever touched from the timer's own thread: start()/stop() are
        // thread-confined because they go through Object::startTimer()/killTimer(), and timerEvent()
        // is delivered by that same thread's event loop. Adding a mutex would only paper over misuse
        // that the thread-confinement rules already forbid.
        //! Emitted, on the timer's own thread, each time the interval elapses. Private, handed out
        //! by getTimeout() as a view: firing a timer's timeout is the timer's job, and a caller
        //! able to emit it directly would be reporting an expiry that never happened.
        Signal<> mTimeout;

        int mInterval { 0 };        //!< The configured interval, in milliseconds.
        int mTimerId { -1 };        //!< The underlying Object timer id, or -1 if inactive.
        bool mSingleShot { false }; //!< True if the timer stops itself after firing once.
        bool mActive { false };     //!< True while the timer is running.
    };

    //! Fires a single-shot timer executing a functor after specified delay.
    template <typename Functor> void Timer::singleShot
        (
        int aMsec,        //!< Delay in milliseconds.
        Functor aFunctor  //!< Callable to run.
        )
    {
        aMsec = checkInterval( "Timer::singleShot", aMsec );

        //! Self-deleting helper that fires its functor once when its timer expires.
        class SingleShotHelper : public Object
        {
        public:
            SingleShotHelper
                (
                int aMs,
                Functor aFn
                )
                : mFn( std::move( aFn ) )
            {
                mId = startTimer( aMs );
            }

            //! @return the timer id, or -1 if the timer could not be started.
            int timerId() const
            {
                return mId;
            }

        protected:
            virtual void timerEvent
                (
                TimerEvent* aEvent
                ) override
            {
                if( !aEvent || aEvent->timerId() != mId )
                {
                    return;
                }

                // Kill before running the functor, so "single shot" holds even if the functor takes
                // longer than the interval: deleteLater() only takes effect on the next pass of the
                // loop, and until it does the timer is still registered and still due.
                killTimer( mId );
                mId = -1;

                mFn();
                deleteLater();
            }

        private:
            Functor mFn;
            int mId { -1 };
        };

        auto* helper = new SingleShotHelper( aMsec, aFunctor );
        if( helper->timerId() == -1 )
        {
            // Nothing will ever fire it -- no event loop on this thread, most likely -- so reclaim
            // the helper rather than leaking it.
            delete helper;
        }
    }

    //! Run a functor once on a context object's thread after a delay.
    template <typename Functor> void Timer::singleShot
        (
        int aMsec,               //!< Delay in milliseconds.
        const Object* aContext,  //!< Object whose thread will run the functor.
        Functor aFunctor         //!< Callable to run.
        )
    {
        // Checked before the context is, as Qt does: the warning is about what the caller passed,
        // so it is worth reporting whether or not there is a context to run it against.
        aMsec = checkInterval( "Timer::singleShot", aMsec );

        if( !aContext )
        {
            return;
        }

        const std::shared_ptr<ThreadData> contextData = aContext->threadData();
        const std::weak_ptr<int> contextLife = aContext->mLife;
        if( !contextData || contextData->thread() == nullptr || contextLife.expired() )
        {
            // Detached object: no loop would ever deliver the timer, so there is nothing to arm.
            return;
        }

        //! Self-deleting helper that arms itself on the context's thread and fires once.
        class SingleShotContextHelper : public Object
        {
        public:
            SingleShotContextHelper
                (
                std::shared_ptr<ThreadData> aOwnerData,
                std::weak_ptr<int> aContextLife,
                int aMs,
                Functor aFn
                )
                : Object( std::move( aOwnerData ) )
                , mContextLife( std::move( aContextLife ) )
                , mFn( std::move( aFn ) )
                , mInterval( aMs )
            {
            }

            //! Register the timer. Must run on this helper's own thread.
            //!
            //! Public only so the posted task below can target it; it is not part of any API.
            void arm()
            {
                if( mContextLife.expired() )
                {
                    delete this;
                    return;
                }
                mId = startTimer( mInterval );
                if( mId == -1 )
                {
                    // Nothing will ever fire, so reclaim the helper rather than leaking it.
                    delete this;
                }
            }

        protected:
            virtual void timerEvent
                (
                TimerEvent* aEvent
                ) override
            {
                if( !aEvent || aEvent->timerId() != mId )
                {
                    return;
                }

                killTimer( mId );  // see SingleShotHelper::timerEvent() for why this is not deferred
                mId = -1;

                if( !mContextLife.expired() )
                {
                    mFn();
                }
                deleteLater();
            }

        private:
            std::weak_ptr<int> mContextLife;
            Functor mFn;
            int mInterval { 0 };
            int mId { -1 };
        };

        // The helper is born on the context's thread, so its startTimer() lands in the same mailbox
        // that will deliver the result. Only the *call* to startTimer() still has to happen there,
        // which is what the hop below is for -- Qt does the same in
        // QSingleShotTimer::startTimerForReceiver(), arming directly when the receiver is on the
        // current thread and posting to start it there otherwise.
        auto* helper = new SingleShotContextHelper( contextData, contextLife, aMsec, aFunctor );

        if( Object::isCurrentThread( contextData ) )
        {
            helper->arm();  // may delete itself; do not touch `helper` afterwards
        }
        else if( !Object::dispatchMetaCallTo( contextData, helper, [helper]()
            {
                helper->arm();
            } ) )
        {
            // The target thread has no dispatcher, so the call was refused rather than queued into
            // something nothing will drain. Reclaim the helper here.
            delete helper;
        }
    }

    //! Call a member function once on the receiver's thread after a delay.
    template <typename Receiver, typename MemberFunc> void Timer::singleShot
        (
        int aMsec,                    //!< Delay in milliseconds.
        const Receiver* aReceiver,    //!< Target receiver object.
        MemberFunc aMethod            //!< Member function pointer to execute.
        )
    {
        if( !aReceiver )
        {
            return;
        }
        auto bound = [aReceiver, aMethod]()
            {
                ( const_cast<Receiver*>( aReceiver )->*aMethod )();
            };
        singleShot( aMsec, static_cast<const Object*>( aReceiver ), bound );
    }
}

#endif // QT_LIKE_SIGNAL_TIMER_HPP
