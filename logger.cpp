#include <QDebug>
#include <QMetaMethod>
#include <QMessageBox>
#include <cstdio>
#include <cstdlib>
#include "logger.h"


static bool loggerInstanceSetBefore = false;
static Logger *loggerInstance = nullptr;

void loggerCallback(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    Q_UNUSED(context);
    switch (type) {
    case QtDebugMsg:
        Logger::log("qt", "debug", msg);
        break;
    case QtInfoMsg:
        Logger::log("qt", "info", msg);
        break;
    case QtWarningMsg:
        Logger::log("qt", "warn", msg);
        break;
    case QtCriticalMsg:
        Logger::log("qt", "crit", msg);
        break;
    case QtFatalMsg:
        Logger::log("qt", "fatal", msg);
        Logger::fatalMessage();
    }
}

Logger::Logger(QObject *owner) : QObject(owner)
{
    if (loggerInstanceSetBefore)
        std::abort();
    qInstallMessageHandler(loggerCallback);
    immediateMode = false;
    flushTimer = new QTimer(this);
    connect(flushTimer, &QTimer::timeout,
            this, &Logger::flushMessages);
}

Logger::~Logger()
{
    qInstallMessageHandler(nullptr);
    if (logFileStream) {
        delete logFileStream;
        logFileStream = nullptr;
    }
    if (logFile) {
        delete logFile;
        logFile = nullptr;
    }
    loggerInstance = nullptr;
}

Logger *Logger::singleton()
{
    // Only create an instance if it hasn't done so before
    if (!loggerInstanceSetBefore && !loggerInstance) {
        loggerInstance = new Logger();
        loggerInstanceSetBefore = true;
    }
    return loggerInstance;
}

// The log buffer class has likely been moved to
// its own separate thread, so remotely invoke
// the respective log methods.
void Logger::log(QString line)
{
    Logger *log = singleton();
    if (!log)
        return;
    QMetaObject::invokeMethod(log, "makeLog",
                              Qt::QueuedConnection,
                              Q_ARG(QString, line));
}

void Logger::log(QString prefix, QString message)
{
    Logger *log = singleton();
    if (!log)
        return;
    QMetaObject::invokeMethod(log, "makeLogPrefixed",
                              Qt::QueuedConnection,
                              Q_ARG(QString, prefix),
                              Q_ARG(QString, message));
}

void Logger::log(QString prefix, QString level, QString message)
{
    Logger *log = singleton();
    if (!log)
        return;
    QMetaObject::invokeMethod(log, "makeLogDescriptively",
                              Qt::QueuedConnection,
                              Q_ARG(QString, prefix),
                              Q_ARG(QString, level),
                              Q_ARG(QString, message));
}

void Logger::logs(const QStringList &strings)
{
    log(strings.join(' '));
}

void Logger::logs(QString prefix, const QStringList &strings)
{
    log(prefix, strings.join(' '));
}

void Logger::logs(QString prefix, QString level, const QStringList &strings)
{
    log(prefix, level, strings.join(' '));
}

void Logger::fatalMessage()
{
    // Oops!  Something went very wrong!
    // Try to flush anything pending to stderr and abort
    if (loggerInstance) {
        for (auto i : loggerInstance->pendingMessages)
            std::fprintf(stderr, "%s\n", i.toLocal8Bit().data());
    }
    std::abort();
}

void Logger::setLogFile(QString fileName)
{
    if (logFileName == fileName)
        return;
    logFileName = fileName;
    if (fileName.isEmpty()) {
        log("logger", "log file closed");
        if (logFileStream) {
            delete logFileStream;
            logFileStream = nullptr;
        }
        if (logFile) {
            delete logFile;
            logFile = nullptr;
        }
        return;
    }
    logFile = new QFile(fileName);
    if (!logFile->open(QFile::WriteOnly))
        return;

    logs("logger", {"log file", logFileName, "opened for writing"});
    logFileStream = new QTextStream(logFile);
    logFileStream->setCodec("UTF-8");
    logFileStream->setGenerateByteOrderMark(true);
}

void Logger::setLoggingEnabled(bool enabled)
{
    if (enabled && !loggingEnabled) {
        loggingEnabled = true;
        makeLogPrefixed("logger", "enabling logging");
        if (!immediateMode)
            flushTimer->start();
    } else if (!enabled && loggingEnabled) {
        makeLogPrefixed("logger", "disabling logging");
        flushMessages();
        flushTimer->stop();
        loggingEnabled = false;
    }
}

void Logger::setFlushTime(int msec)
{
    if (msec <= 0) {
        flushTimer->stop();
        if (!immediateMode)
            flushMessages();
        immediateMode = true;
        return;
    } else {
        immediateMode = false;
        flushTimer->setInterval(std::max(100, msec));
        flushTimer->start();
    }
}

void Logger::flushMessages()
{
    if (pendingMessages.isEmpty())
        return;
    if (logFileStream) {
        *logFileStream << pendingMessages.join('\n') << '\n';
        logFileStream->flush();
    }
    emit logMessageBuffer(pendingMessages);
    pendingMessages.clear();
}

void Logger::makeLog(QString line)
{
    if (!loggingEnabled)
        return;
    line = line.trimmed();
    // If you're encountering early or fantastic errors, uncomment this line:
    //fprintf(stderr, "%s\n",  line.toLocal8Bit().constData());
    if (immediateMode) {
        emit logMessage(line);
        if (logFileStream) {
            *logFileStream << line << '\n';
            logFileStream->flush();
        }
    } else {
        pendingMessages.append(line);
    }
}

void Logger::makeLogPrefixed(QString prefix, QString message)
{
    makeLog(QString("[%1] %2").arg(prefix, message));
}

void Logger::makeLogDescriptively(QString prefix, QString level, QString message)
{
    makeLog(QString("[%1] %2: %3").arg(prefix, level, message));
}



LogStream::LogStream(QString prefix, QString level) : buffer(),
    prefix(prefix), level(level), stream(&buffer)
{

}

LogStream::~LogStream()
{
    if (buffer.isEmpty())
        return;
    if (prefix.isEmpty() && level.isEmpty())
        Logger::log(buffer);
    else if (level.isEmpty())
        Logger::log(prefix, buffer);
    else
        Logger::log(prefix, level, buffer);
}

LogStream &LogStream::operator<<(const char *a)
{
    stream << a;
    return *this;
}

LogStream &LogStream::operator<<(const QString &a)
{
    stream << a;
    return *this;
}

LogStream &LogStream::operator<<(const QVariant &a)
{
    if (a.canConvert(QMetaType::QVariantMap)) {
        stream << "{";
        auto list = a.toMap();
        int count = list.count();
        for (auto it = list.constBegin(); it != list.constEnd(); it++, count--) {
            stream << '"' << it.key() << "\":";
            *this << it.value();
            if (count > 1)
                stream << ", ";
        }
        *this << "}";
    } else if (a.canConvert(QMetaType::QVariantList)) {
        stream << "[";
        auto list = a.toList();
        int count = list.count();
        for (int i = 0; i < count; i++) {
            *this << list.value(i);
            if (i < count - 1)
                stream << ", ";
        }
        stream << "]";
    } else if (a.canConvert(QMetaType::QString)) {
        stream << a.toString();
    } else {
        stream << "(unserializable)";
    }
    return *this;
}
