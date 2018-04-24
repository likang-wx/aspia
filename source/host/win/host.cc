//
// PROJECT:         Aspia
// FILE:            host/win/host.cc
// LICENSE:         GNU General Public License 3
// PROGRAMMERS:     Dmitry Chapyshev (dmitry@aspia.ru)
//

#include "host/win/host.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <QCoreApplication>
#include <QDebug>

#include "host/win/host_process.h"
#include "ipc/ipc_channel.h"
#include "ipc/ipc_server.h"
#include "network/network_channel.h"

namespace aspia {

Host::Host(proto::auth::SessionType session_type, NetworkChannel* network_channel, QObject* parent)
    : QObject(parent),
      session_type_(session_type),
      network_channel_(network_channel)
{
    Q_ASSERT(!network_channel_.isNull());

    connect(network_channel_, &NetworkChannel::disconnected,
            network_channel_, &NetworkChannel::deleteLater);
}

Host::~Host()
{
    stop();
}

void Host::start()
{
    state_ = StartingState;

    connect(network_channel_, &NetworkChannel::disconnected, this, &Host::stop);
    connect(network_channel_, &NetworkChannel::messageWritten, this, &Host::networkMessageWritten);
    connect(network_channel_, &NetworkChannel::messageReceived, this, &Host::networkMessageReceived);

    attach_timer_id_ = startTimer(std::chrono::minutes(1));
    if (!attach_timer_id_)
    {
        qWarning("Could not start the timer");
        stop();
    }
    else
    {
        attachSession(WTSGetActiveConsoleSessionId());
    }
}

void Host::stop()
{
    if (state_ == StoppedState)
        return;

    dettachSession();

    state_ = StoppedState;

    if (attach_timer_id_)
    {
        killTimer(attach_timer_id_);
        attach_timer_id_ = 0;
    }

    emit finished(this);
}

void Host::sessionChanged(quint32 event, quint32 session_id)
{
    if (state_ != AttachedState && state_ != DetachedState)
        return;

    switch (event)
    {
        case WTS_CONSOLE_CONNECT:
            attachSession(session_id);
            break;

        case WTS_CONSOLE_DISCONNECT:
            dettachSession();
            break;

        default:
            break;
    }
}

void Host::timerEvent(QTimerEvent* event)
{
    if (event->timerId() == attach_timer_id_)
        stop();
}

void Host::networkMessageWritten(int message_id)
{
    if (!ipc_channel_.isNull())
        ipc_channel_->readMessage();
}

void Host::networkMessageReceived(const QByteArray& buffer)
{
    if (!ipc_channel_.isNull())
        ipc_channel_->writeMessage(-1, buffer);
}

void Host::ipcMessageWritten(int message_id)
{
    if (!network_channel_.isNull())
        network_channel_->readMessage();
}

void Host::ipcMessageReceived(const QByteArray& buffer)
{
    if (!network_channel_.isNull())
        network_channel_->writeMessage(-1, buffer);
}

void Host::ipcServerStarted(const QString& channel_id)
{
    Q_ASSERT(state_ == StartingState);
    Q_ASSERT(session_process_.isNull());

    session_process_ = new HostProcess(this);

    session_process_->setSessionId(session_id_);
    session_process_->setProgram(
        QCoreApplication::applicationDirPath() + QLatin1String("/aspia_host.exe"));

    QStringList arguments;

    arguments << "--channel_id" << channel_id;
    arguments << "--session_type";

    switch (session_type_)
    {
        case proto::auth::SESSION_TYPE_DESKTOP_MANAGE:
            session_process_->setAccount(HostProcess::Account::System);
            arguments << "desktop_manage";
            break;

        case proto::auth::SESSION_TYPE_DESKTOP_VIEW:
            session_process_->setAccount(HostProcess::Account::System);
            arguments << "desktop_view";
            break;

        case proto::auth::SESSION_TYPE_FILE_TRANSFER:
            session_process_->setAccount(HostProcess::Account::User);
            arguments << "file_transfer";
            break;

        default:
            qFatal("Unknown session type: %d", session_type_);
            break;
    }

    session_process_->setArguments(arguments);

    connect(session_process_, &HostProcess::errorOccurred, this, &Host::stop);
    connect(session_process_, &HostProcess::finished, this, &Host::dettachSession);

    session_process_->start();
}

void Host::ipcNewConnection(IpcChannel* channel)
{
    Q_ASSERT(channel);
    Q_ASSERT(attach_timer_id_);

    killTimer(attach_timer_id_);
    attach_timer_id_ = 0;

    ipc_channel_ = channel;

    connect(ipc_channel_, &IpcChannel::disconnected, this, &Host::dettachSession);
    connect(ipc_channel_, &IpcChannel::messageReceived, this, &Host::ipcMessageReceived);
    connect(ipc_channel_, &IpcChannel::messageWritten, this, &Host::ipcMessageWritten);

    state_ = AttachedState;

    ipc_channel_->readMessage();
    network_channel_->readMessage();
}

void Host::attachSession(quint32 session_id)
{
    state_ = StartingState;
    session_id_ = session_id;

    IpcServer* ipc_server = new IpcServer(this);

    connect(ipc_server, &IpcServer::started, this, &Host::ipcServerStarted);
    connect(ipc_server, &IpcServer::finished, ipc_server, &IpcServer::deleteLater);
    connect(ipc_server, &IpcServer::newConnection, this, &Host::ipcNewConnection);
    connect(ipc_server, &IpcServer::errorOccurred, this, &Host::stop);

    ipc_server->start();
}

void Host::dettachSession()
{
    if (state_ == StoppedState || state_ == DetachedState)
        return;

    state_ = DetachedState;

    if (!session_process_.isNull())
    {
        session_process_->kill();
        delete session_process_;
    }

    delete ipc_channel_;

    if (session_type_ == proto::auth::SESSION_TYPE_FILE_TRANSFER)
    {
        // The file transfer session ends when the user quits.
        stop();
        return;
    }

    attach_timer_id_ = startTimer(std::chrono::minutes(1));
    if (!attach_timer_id_)
    {
        qWarning("Could not start the timer");
        stop();
    }
}

} // namespace aspia