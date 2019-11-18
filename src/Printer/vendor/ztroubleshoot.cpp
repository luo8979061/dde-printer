/*
 * Copyright (C) 2019 ~ 2019 Deepin Technology Co., Ltd.
 *
 * Author:     Wei xie <xiewei@deepin.com>
 *
 * Maintainer: Wei xie  <xiewei@deepin.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "ztroubleshoot.h"
#include "cupsattrnames.h"
#include "common.h"
#include "cupsconnection.h"
#include "qtconvert.h"
#include "ztroubleshoot_p.h"
#include "zdrivermanager.h"
#include "zcupsmonitor.h"
#include "zjobmanager.h"

#ifdef CONSOLE_CMD
#include "zprintermanager.h"
#else
#include "dprintermanager.h"
#endif

#include <QTcpSocket>
#include <QUrl>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QThread>

static bool isFilterMissing(const QString &filter)
{
    char const *filterPath[] = {"/usr/lib/cups/filter/", "/usr/lib64/cups/filter/", nullptr};
    char const *builtinFilters[] = { ":", ".", "[", "alias", "bind", "break", "cd",
                     "continue", "declare", "echo", "else", "eval",
                     "exec", "exit", "export", "fi", "if", "kill", "let",
                     "local", "popd", "printf", "pushd", "pwd", "read",
                     "readonly", "set", "shift", "shopt", "source",
                     "test", "then", "trap", "type", "ulimit", "umask",
                     "unalias", "unset", "wait", "-", nullptr};

    if ("%" == filter.right(1))
        return false;

    for (int i=0;builtinFilters[i];i++) {
        if (filter == builtinFilters[i]) {
            return false;
        }
    }

    if (filter.startsWith("/") && QFile::exists(filter))
        return false;

    for (int i=0;filterPath[i];i++){
        if (QFile::exists(QString(filterPath[i]) + filter)){
            return false;
        }
    }

    return true;
}

static QStringList getDirectDevices()
{
    QStringList uris;
    vector<string> inSechemes = {"hp", "usb"};
    map<string, map<string, string>> devs;
    map<string, map<string, string>>::iterator itmap;

    try {
        devs = g_cupsConnection->getDevices(nullptr, &inSechemes, 0, CUPS_TIMEOUT_DEFAULT);
    }catch(const std::exception &ex) {
        qWarning() << "Got execpt: " << QString::fromUtf8(ex.what());
        return uris;
    }

    for(itmap=devs.begin();itmap != devs.end();itmap++){
        uris << STQ(itmap->first);
    }

    return uris;
}

CheckCupsServer::CheckCupsServer(QObject *parent)
    : TroubleShootJob("", parent)
{}

QString CheckCupsServer::getJobName()
{
    return tr("Check cups server");
}

//诊断本地Cups服务是否正常
bool CheckCupsServer::isPass()
{
    Connection c;

    emit signalStateChanged(TStat_Running, tr("Starting check cups server"));

    try {
        c.init(cupsServer(), ippPort(), 0);
    }catch(const std::exception &ex) {
        qWarning() << "Got execpt: " << QString::fromUtf8(ex.what());
        m_strMessage = tr("Cups server is invaild, err: ") + QString::fromUtf8(ex.what());
        emit signalStateChanged(TStat_Fail, m_strMessage);
        return false;
    }

    emit signalStateChanged(TStat_Suc, tr("Cups server is vaild"));
    return true;
}

CheckDriver::CheckDriver(const QString &printerName, QObject *parent)
    : TroubleShootJob(printerName, parent)
{
}

//诊断驱动文件是否正常
bool CheckDriver::isPass()
{
    QString strPPD;
    QFile ppdFile;
    char buf[1024];
    QStringList depends;

    emit signalStateChanged(TStat_Running, tr("Starting check driver"));

    if (m_printerName.isEmpty()) {
        emit signalStateChanged(TStat_Suc, tr("Success"));
        return true;
    }

    strPPD = getPrinterPPD(m_printerName.toUtf8().data());
    ppdFile.setFileName(strPPD);
    if (!ppdFile.open(QFile::ReadOnly)) {
        qWarning() << strPPD << "not found";
        m_strMessage = tr("Driver file %1 not found").arg(strPPD);
        emit signalStateChanged(TStat_Fail, m_strMessage);
        return false;
    }

    while (ppdFile.readLine(buf, sizeof(buf)) > 0) {
        QString str = buf;
        if (str.startsWith("*cupsFilter:")) {
            str = str.trimmed();
            QString filter = str.split(" ").last().replace("\"", "");
            qDebug() << strPPD << " filter: " << filter;
            if (isFilterMissing(filter)) {
                m_strMessage = tr("Driver filter %1 not found").arg(filter);
                emit signalStateChanged(TStat_Fail, m_strMessage);
                return false;
            }
        }
    }

    depends = g_driverManager->getDriverDepends(strPPD.toUtf8().data());
    foreach (QString package, depends) {
        if (!isPackageExists(package)) {
            qWarning() << package << "is not exists";
            m_strMessage = tr("%1 is not install, can't printer");
            emit signalStateChanged(TStat_Fail, m_strMessage);
            return false;
        }
    }

    emit signalStateChanged(TStat_Suc, tr("Driver is vaild"));
    return true;
}

QString CheckDriver::getJobName()
{
    return tr("Check driver");
}

CheckConnected::CheckConnected(const QString &printerName, QObject *parent)
    : TroubleShootJob(printerName, parent)
{
}

QString CheckConnected::getJobName()
{
    return tr("Check connection");
}

//检查打印机能否连接
bool CheckConnected::isPass()
{
    unsigned short iPort;
    QUrl url;
    QString strUri, strHost;
    QString pingCmd, strOut, strErr, strScheme;
    QStringList uriDepends;

    emit signalStateChanged(TStat_Running, tr("Starting check connected"));

    if (m_printerName.isEmpty()) {
        emit signalStateChanged(TStat_Suc, tr("Success"));
        return true;
    }

    strUri = getPrinterUri(m_printerName.toUtf8().data());

    url.setUrl(strUri);
    strScheme = url.scheme();
    iPort = static_cast<unsigned short>(url.port(0));
    if (0 == iPort) {
        if ("ipp" == strScheme || "http" == strScheme || "https" == strScheme) {
            iPort = 631;
        } else if ("lpd" == strScheme){
            iPort = 515;
        } else if ("socket" == strScheme) {
            iPort = 9100;
        }
    }

    strHost = getHostFromUri(strUri);
    qDebug() << strUri << "scheme:" << strScheme << "host:" << strHost << "port:" << iPort;

    m_strMessage = reslovedHost(strHost);
    if (!m_strMessage.isEmpty()) {
        qWarning() << strHost << "is not found";
        emit signalStateChanged(TStat_Fail, m_strMessage);
        return false;
    }

    if (m_bQuit) return false;

    if (0 != iPort && !strHost.isEmpty()){
        QTcpSocket socket;
        socket.connectToHost(strHost, iPort);
        if (!socket.waitForConnected(3000)) {
            qWarning() << "Can't connect to uri: " << strUri << socket.errorString();
            m_strMessage = tr("Can't connect printer host: %1, error string: %2").arg(strHost).arg(socket.errorString());
            emit signalStateChanged(TStat_Fail, m_strMessage);
            return false;
        }
    } else if (strHost.isEmpty() && ("hp" == strScheme || "usb" == strScheme)) {
        QStringList uris = getDirectDevices();
        if (!uris.contains(strUri)) {
            qWarning() << "Not found" << strUri;
            m_strMessage = m_printerName + tr(" is not connected, uri is: ") + strUri;
            emit signalStateChanged(TStat_Fail, m_strMessage);
            return false;
        }
    } else if ("file" == strScheme) {
        QString filePath = strUri.right(strUri.length()-7);
        QFileInfo fileInfo(filePath);
        if (!fileInfo.absoluteDir().exists()) {
            qWarning() << fileInfo.absoluteDir().path() << "not exists";
            m_strMessage = tr("It is a file printer, %1 not exists").arg(fileInfo.absoluteDir().path());
            emit signalStateChanged(TStat_Fail, m_strMessage);
            return false;
        }
    } else if (!strHost.isEmpty()) {
        pingCmd = QString("ping %1 -c 3").arg(strHost);
        shellCmd(pingCmd, strOut, strErr, 5000);
        if (!strOut.contains("ttl=")) {
            qWarning() << "Can't connect printer host: " << strHost;
            m_strMessage = tr("Connected to printer's %1 failed") + strHost;
            emit signalStateChanged(TStat_Fail, m_strMessage);
            return false;
        }
    }

    emit signalStateChanged(TStat_Suc, tr("Printer connected is vaild"));
    return true;
}

CheckAttributes::CheckAttributes(const QString &printerName, QObject *parent)
    : TroubleShootJob(printerName, parent)
{
}

QString CheckAttributes::getJobName()
{
    return tr("Check printer settings");
}

bool CheckAttributes::isPass()
{
    map<string, string> attrs;
    QString strState, strIsAccept;
    vector<string> reqs{CUPS_OP_STATE, CUPS_OP_ISACCEPT};

    emit signalStateChanged(TStat_Running, tr("Starting check printer settings"));

    if (m_printerName.isEmpty()) {
        emit signalStateChanged(TStat_Suc, tr("Success"));
        return true;
    }

    try {
        attrs = g_cupsConnection->getPrinterAttributes(m_printerName.toUtf8().data(), nullptr, &reqs);
    }catch(const std::exception &ex) {
        qWarning() << "Got execpt: " << QString::fromUtf8(ex.what());
        emit signalStateChanged(TStat_Fail, tr("Get printer attributes failed, err: ") + QString::fromUtf8(ex.what()));
        return true;
    }

    //检查打印机是否启用
    strState = attrValueToQString(attrs[CUPS_OP_STATE]);
    if (IPP_PRINTER_STOPPED == strState.toInt()) {
        qWarning() << m_printerName << "is disable";
        m_strMessage = tr("%1 is disable").arg(m_printerName);
        emit signalStateChanged(TStat_Fail, m_strMessage);
        return false;
    }

    //检查打印机是否接受打印任务
    strIsAccept = attrs[CUPS_OP_ISACCEPT].c_str();
    if ("b0" == strIsAccept) {
        qWarning() << m_printerName << "is not accept jobs";
        m_strMessage = m_printerName + tr("is not accept jobs");
        emit signalStateChanged(TStat_Fail, m_strMessage);
        return false;
    }

    emit signalStateChanged(TStat_Suc, tr("Printer settings is ok"));
    return true;
}

PrinterTestJob::PrinterTestJob(const QString &printerName, QObject *parent, bool bSync)
    :TroubleShootJob(printerName, parent),
      m_jobId(-1)
{
    m_eventLoop = nullptr;
    m_bSync = bSync;
}

PrinterTestJob::~PrinterTestJob()
{
    stop();
    if (m_eventLoop)
        m_eventLoop->deleteLater();
}

QString PrinterTestJob::getJobName()
{
    return tr("Check print test page");
}

void PrinterTestJob::stop()
{
    TroubleShootJob::stop();

    disconnect(g_cupsMonitor, &CupsMonitor::signalJobStateChanged, this, &PrinterTestJob::slotJobStateChanged);
    if (m_eventLoop)
        m_eventLoop->exit(0);
}

void PrinterTestJob::findRunningJob()
{
    map<int, map<string, string>> jobs;
    map<int, map<string, string>>::iterator itJobs;

    if (0 != g_jobManager->getJobs(jobs, WHICH_JOB_RUNING))
        return ;

    for (itJobs=jobs.begin();itJobs!=jobs.end();itJobs++) {
        map<string, string> jobinfo = itJobs->second;
        QString uri = attrValueToQString(jobinfo[JOB_ATTR_URI]);
        int iState = attrValueToQString(jobinfo[JOB_ATTR_STATE]).toInt();

        if (IPP_JSTATE_PROCESSING == iState && m_printerName == getPrinterNameFromUri(uri)) {
            m_jobId = itJobs->first;
            QMap<QString, QVariant> job;
            map<string, string>::const_iterator itjob;

            job.insert(JOB_ATTR_ID, m_jobId);
            for (itjob=jobinfo.begin();itjob!=jobinfo.end();itjob++) {
                job.insert(STQ(itjob->first), attrValueToQString(itjob->second));
            }

            m_strMessage = g_cupsMonitor->getJobNotify(job);
            emit signalStateChanged(TStat_Update, m_strMessage);
            break;
        }
    }
}

bool PrinterTestJob::isPass()
{
    emit signalStateChanged(TStat_Running, tr("Starting check by print test page"));

    connect(g_cupsMonitor, &CupsMonitor::signalJobStateChanged, this, &PrinterTestJob::slotJobStateChanged);
    if (!g_cupsMonitor->isRunning())
        g_cupsMonitor->start();

    //同步模式用在故障排查中，故障排查的时候如果已经有任务，则不重新添加
    if (m_bSync)
        findRunningJob();

    if (-1 == m_jobId) {
        m_strMessage = g_jobManager->printTestPage(m_printerName.toUtf8().data(), m_jobId);
        if (!m_strMessage.isEmpty()){
            emit signalStateChanged(TStat_Fail, m_strMessage);
            return false;
        }
    }

    if (m_bSync) {
        m_eventLoop = new QEventLoop();
        qInfo() << "Printer test job runing" << m_jobId;
        return 0 == m_eventLoop->exec();
    }

    return true;
}

void PrinterTestJob::slotJobStateChanged(int id, int state, const QString &message)
{
    if (id != m_jobId) return;

    m_strMessage = g_cupsMonitor->getStateString(state) + " " + message;

    switch (state) {
    case IPP_JSTATE_PENDING:
    case IPP_JSTATE_PROCESSING:
    case IPP_JSTATE_HELD:
        state = TStat_Update;
        break;
    case IPP_JSTATE_COMPLETED:
        state = TStat_Suc;
        break;
    case IPP_JSTATE_STOPPED:
    case IPP_JSTATE_CANCELED:
    case IPP_JSTATE_ABORTED:
    default:
        state = TStat_Fail;
        break;
    }

    emit signalStateChanged(state, m_strMessage);

    if (m_eventLoop && state >= TStat_Suc) {
        m_eventLoop->exit(TStat_Suc==state?0:-1);
    }
}

TroubleShoot::TroubleShoot(const QString &printerName, QObject *parent)
    : TaskInterface(TASK_TroubleShoot, parent)
{
    m_printerName = printerName;

    addJob(new CheckCupsServer(this));
    addJob(new CheckDriver(m_printerName, this));
    addJob(new CheckConnected(m_printerName, this));
    addJob(new CheckAttributes(m_printerName, this));
}

TroubleShoot::~TroubleShoot()
{
    qInfo() << "";
    stop();
}

int TroubleShoot::addJob(TroubleShootJob *job)
{
    if (!job)
        return -1;

    m_jobs.append(job);

    return 0;
}

QList<TroubleShootJob*> TroubleShoot::getJobs()
{
    return m_jobs;
}

void TroubleShoot::stop()
{
    qInfo() << "";
    foreach (TroubleShootJob *job, m_jobs) {
        job->stop();
    }

    TaskInterface::stop();
}

int TroubleShoot::doWork()
{
    int i=0;
    for (;i<m_jobs.count() && !m_bQuit;i++) {
        bool bPass = m_jobs[i]->isPass();
        emit signalUpdateProgress(i, m_jobs[i]->getMessage());

        qInfo() << m_jobs[i]->getJobName() << m_jobs[i]->getMessage();
        if (!bPass) {
            return -1;
        }
    }

    return 0;
}