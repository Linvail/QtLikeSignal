#ifndef QT_LIKE_SIGNAL_QT6_PERF_OBJECTS_H
#define QT_LIKE_SIGNAL_QT6_PERF_OBJECTS_H

//! @file
//!
//! The QObject subclass the Qt6 benchmarks emit from.
//!
//! It lives in a header rather than beside the benchmarks because a signal only exists once moc has
//! generated its body. Keeping the class here lets the build run moc into an ordinary .cpp that is
//! compiled and linked like any other source, which makes the dependency explicit to waf. The
//! alternative -- declaring the class in the .cpp and `#include`-ing a generated .moc at the bottom
//! -- relies on the build system happening to run moc before the compile, which is exactly the kind
//! of ordering that works until it doesn't.
//!
//! Only ever included by the Qt6 translation unit. Qt defines `emit` as an empty macro, so pulling
//! Qt headers into a file that also calls `sig.emit( 1 )` on QtLikeSignal would turn
//! those into syntax errors.

#include <QtCore/QObject>

//! Emits a signal on demand, so a benchmark can drive Qt's dispatch machinery directly.
class Qt6PerfSender : public QObject
{
    Q_OBJECT

public:
    //! Constructs the sender.
    explicit Qt6PerfSender
        (
        QObject* aParent = nullptr   //!< Optional parent.
        )
        : QObject( aParent )
    {
    }

    //! Fires the signal with @p aValue.
    //!
    //! A plain method rather than `emit` at the call site, purely so the benchmark bodies read the
    //! same as their QtLikeSignal counterparts.
    void fire
        (
        int aValue   //!< Value to pass to connected slots.
        )
    {
        emit fired( aValue );
    }

signals:
    //! Emitted by fire().
    void fired( int aValue );
};

//! Plain receiver, used where a benchmark needs a QObject to own a connection.
class Qt6PerfReceiver : public QObject
{
    Q_OBJECT

public:
    //! Constructs the receiver.
    explicit Qt6PerfReceiver
        (
        QObject* aParent = nullptr   //!< Optional parent.
        )
        : QObject( aParent )
    {
    }
};

#endif // QT_LIKE_SIGNAL_QT6_PERF_OBJECTS_H
