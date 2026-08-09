#ifndef TIMER_H
#define TIMER_H

#include "Object.h"
#include "Signal.h"

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
        Timer();

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
        template <typename Functor>
        static void singleShot
            (
            int aMsec,
            const Object* aContext,
            Functor aFunctor
            );

        //! Fires a single-shot timer executing a member function on receiver object. Receiver is
        //! the receiver object type and MemberFunc the member function pointer type.
        template <typename Receiver, typename MemberFunc>
        static void singleShot
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
        Functor aFunctor  //!< Slot function to execute.
        )
    {
        //! Self-deleting helper that fires functor once when its timer expires.
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

        protected:
            virtual void timerEvent
                (
                TimerEvent* aEvent
                ) override
            {
                if( aEvent->timerId() == mId )
                {
                    mFn();
                    deleteLater();
                }
            }

        public:
            int timerId() const
            {
                return mId;
            }

        private:
            Functor mFn;
            int mId { -1 };
        };
        auto* helper = new SingleShotHelper( aMsec, aFunctor );
        if( helper->timerId() == -1 )
        {
            delete helper;
        }
    }

    //! Fires a single-shot timer executing a functor in context object's thread.
    template <typename Functor>
    void Timer::singleShot
        (
        int aMsec,                  //!< Delay in milliseconds.
        const Object* aContext,    //!< Target context Object.
        Functor aFunctor            //!< Slot function to execute.
        )
    {
        if( !aContext )
        {
            return;
        }
        //! Self-deleting helper that arms itself on context's thread and fires functor once.
        class SingleShotContextHelper : public Object
        {
        public:
            SingleShotContextHelper
                (
                int aMs,
                Functor aFn
                )
                : mFn( std::move( aFn ) )
                , mInterval( aMs )
            {
            }

            //! Registers the timer. Must run on this helper's own thread.
            //!
            //! Public only so callLater() can target it; it is not part of any API.
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
                if( aEvent->timerId() == mId )
                {
                    mFn();
                    deleteLater();
                }
            }

        private:
            Functor mFn;
            int mInterval { 0 };
            int mId { -1 };
        };

        auto*    helper = new SingleShotContextHelper( aMsec, aFunctor );
        Object* ctx    = const_cast<Object*>( aContext );

        // startTimer() is thread-confined, so the timer has to be registered on the thread that will
        // deliver its events -- not on whichever thread happens to call singleShot(). This mirrors
        // Qt's QSingleShotTimer::startTimerForReceiver(), which arms directly when the receiver is on
        // the current thread and otherwise moves itself to the receiver's thread and posts an event
        // to start the timer there.
        //
        // The helper was constructed here, so its thread() is this thread.
        if( helper->thread() == ctx->thread() )
        {
            helper->arm(); // may delete itself; do not touch `helper` afterwards
        }
        else
        {
            helper->moveToThread( ctx->thread() );
            Object::callLater( helper, &SingleShotContextHelper::arm );
        }
    }

    //! Fires a single-shot timer executing a member function on receiver object.
    template <typename Receiver, typename MemberFunc>
    void Timer::singleShot
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

#endif // TIMER_H
