//! @file
//!
//! QtMimic::Timer - repeating and single-shot timers, mirroring Qt's QTimer.
//!
//! A Timer is an Object, so it lives in a thread; its timeout signal is emitted by that thread's
//! event loop. The loop is what drives it, so a Timer belonging to a thread that never runs one
//! (a bare adopted thread, say) will never fire.
//!
//! Copyright 2026 by Garmin Ltd. or its subsidiaries.

#ifndef QT_MIMIC_TIMER_HPP
#define QT_MIMIC_TIMER_HPP

#include "Object.hpp"
#include "Signal.hpp"
#include "Thread.hpp"
#include "TimerEvent.hpp"

#include <utility>

namespace QtMimic
{

    //----------------------------------------------------------------
    //! @class Timer
    //!
    //! Repetitive and single-shot timers.
    //!
    //! **No part of this class is thread-safe**, exactly as with Qt's QTimer. A Timer must be used
    //! from the thread it lives in: start()/stop() are thread-confined because they go through
    //! Object::startTimer()/killTimer(), and timeout is emitted by that thread's event loop. To
    //! drive a timer that lives in another thread, hop onto it first -- post a task to that thread
    //! that calls start().
    //!
    //! Usage:
    //! @code
    //!   QtMimic::Timer timer;
    //!   QtMimic::Object::connect( timer.getTimeout(), &receiver, &Receiver::onTick );
    //!   timer.start( 100 );   // every 100 ms, on the thread the timer lives in
    //! @endcode
    //----------------------------------------------------------------
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

        //! Start or restart the timer with an interval of @p aMsec milliseconds.
        //!
        //! **Must be called from this timer's own thread**, because it goes through
        //! Object::startTimer(); see that function for why. Same rule as Qt's QTimer, whose start()
        //! is likewise a plain forward to QObject::startTimer().
        void start
            (
            int aMsec  //!< Interval in milliseconds.
            );

        void start();

        void stop();

        SignalView<>& getTimeout() const;

        //! Run @p aFunctor once, on the calling thread, after @p aMsec milliseconds.
        //!
        //! The calling thread must be running an event loop, since that is what will deliver the
        //! timer; from a thread that is not, this does nothing.
        //! @tparam Functor Any callable taking no arguments.
        template <typename Functor> static void singleShot
            (
            int aMsec,
            Functor aFunctor
            );

        //! Run @p aFunctor once, on @p aContext's thread, after @p aMsec milliseconds.
        //!
        //! Safe to call from any thread. A null context, or a context with no thread, does nothing.
        //! @tparam Functor Any callable taking no arguments.
        template <typename Functor> static void singleShot
            (
            int aMsec,
            const Object* aContext,
            Functor aFunctor
            );

        //! Call @p aMethod on @p aReceiver once, on the receiver's thread, after @p aMsec
        //! milliseconds. A null receiver does nothing.
        //! @tparam Receiver The receiver's type (must derive from Object).
        //! @tparam MemberFunc Pointer to a member function taking no arguments.
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
        // Deliberately unsynchronised, matching QTimer, which has no locking of any kind. Every
        // member here is only ever touched from the timer's own thread: start()/stop() are
        // thread-confined because they go through Object::startTimer()/killTimer(), and timerEvent()
        // is delivered by that same thread's event loop. Adding a mutex would only paper over misuse
        // that the thread-confinement rules already forbid.
        int mInterval { 0 };         //!< The configured interval, in milliseconds.
        int mTimerId { -1 };         //!< The underlying Object timer id, or -1 if inactive.
        bool mSingleShot { false };  //!< True if the timer stops itself after firing once.
        bool mActive { false };      //!< True while the timer is running.

        //! Emitted, on the timer's own thread, each time the interval elapses.
        //!
        //! Private, and handed out only as a view: firing a timer's timeout is the timer's job, and
        //! a caller that could emit it directly would be reporting an expiry that never happened.
        //! Same reasoning as Thread's mStarted/mFinished.
        Signal<> mTimeout;
    };

    //! Run a functor once on the calling thread after a delay.
    template <typename Functor> void Timer::singleShot
        (
        int aMsec,        //!< Delay in milliseconds.
        Functor aFunctor  //!< Callable to run.
        )
    {
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
        if( !aContext )
        {
            return;
        }

        // Read once: the context is alive for the duration of this call (the caller handed it to
        // us), and everything below goes through the Thread's own thread-safe entry points.
        Thread* const contextThread = aContext->thread();
        if( !contextThread )
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
                Thread* aOwner,
                int aMs,
                Functor aFn
                )
                : Object( aOwner )
                , mFn( std::move( aFn ) )
                , mInterval( aMs )
            {
            }

            //! Register the timer. Must run on this helper's own thread.
            //!
            //! Public only so the posted task below can target it; it is not part of any API.
            void arm()
            {
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

                mFn();
                deleteLater();
            }

        private:
            Functor mFn;
            int mInterval { 0 };
            int mId { -1 };
        };

        // The helper is born on the context's thread, so its startTimer() lands in the same mailbox
        // that will deliver the result. Only the *call* to startTimer() still has to happen there,
        // which is what the hop below is for -- Qt does the same in
        // QSingleShotTimer::startTimerForReceiver(), arming directly when the receiver is on the
        // current thread and posting to start it there otherwise.
        auto* helper = new SingleShotContextHelper( contextThread, aMsec, aFunctor );

        if( Thread::currentThread() == contextThread )
        {
            helper->arm();  // may delete itself; do not touch `helper` afterwards
        }
        else if( !contextThread->post( [helper]()
            {
                helper->arm();
            } ) )
        {
            // The target loop has already stopped, so the task was refused rather than queued into
            // something nothing will drain. Reclaim the helper here.
            delete helper;
        }
    }

    //! Call a member function once on the receiver's thread after a delay.
    template <typename Receiver, typename MemberFunc> void Timer::singleShot
        (
        int aMsec,                  //!< Delay in milliseconds.
        const Receiver* aReceiver,  //!< Object to call the member function on.
        MemberFunc aMethod          //!< Member function taking no arguments.
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

} // namespace QtMimic

#endif // QT_MIMIC_TIMER_HPP
