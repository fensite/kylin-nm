/*
 * Copyright (C) 2020 Tianjin KYLIN Information Technology Co., Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/&gt;.
 *
 */

#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "oneconnform.h"
#include "onelancform.h"
#include "wifi-auth-thread.h"
#include "hot-spot/dlghotspotcreate.h"
#include "wireless-security/dlghidewifi.h"
#include "sysdbusregister.h"

#include <algorithm>//sort函数包含的头文件

#include <KWindowEffects>
#include <QFont>
#include <QFontMetrics>
#include <QDir>
#include <QtConcurrent>

QString llname, lwname, hideWiFiConn;
int currentActWifiSignalLv, count_loop;

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    //syslog(LOG_DEBUG, "Using the icon theme named 'ukui-icon-theme-default'");
    //QIcon::setThemeName("ukui-icon-theme-default");

    PrimaryManager();
    toStart();

    // 如果使用Qt::Popup 任务栏不显示且保留X事件如XCB_FOCUS_OUT, 但如果indicator点击鼠标右键触发，XCB_FOCUS_OUT事件依然会失效
    // 如果使用Qt::ToolTip, Qt::Tool + Qt::WindowStaysOnTopHint, Qt::X11BypassWindowManagerHint等flag则会导致X事件失效
    // this->setWindowFlags(Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint);
    //this->setWindowFlags(Qt::FramelessWindowHint | Qt::Popup);//QTool
    this->setWindowFlags(Qt::CustomizeWindowHint | Qt::FramelessWindowHint | Qt::X11BypassWindowManagerHint);//QTool
    this->setAttribute(Qt::WA_TranslucentBackground);//设置窗口背景透明

    // 连接kds的dbus接收rfkill变化的信号&获取当前WIFI状态
    kdsDbus = new QDBusInterface("org.ukui.kds", \
                               "/", \
                               "org.ukui.kds.interface", \
                               QDBusConnection::systemBus());
    QDBusConnection::systemBus().connect(kdsDbus->service(), kdsDbus->path(), kdsDbus->interface(), "signalRfkillStatusChanged", this, SLOT(onRfkillStatusChanged()));

    UseQssFile::setStyle("style.qss");

    editQssString(); //编辑部分控件QSS
    createTopLanUI(); //创建顶部有线网item
    createTopWifiUI(); //创建顶部无线网item
    createOtherUI(); //创建上传下载控件，列表区无item时的说明控件
    createListAreaUI(); //创建列表区域的控件
    createLeftAreaUI(); //创建左侧区域控件

    lcardname = "-1";
    wcardname = "-1";
    llname = "-1";
    lwname = "-1";
    hideWiFiConn = "Connect to Hidden Wi-Fi Network";
    currentActWifiSignalLv = -1;
    count_loop = 0;

    createTrayIcon();
    connect(trayIcon, &QSystemTrayIcon::activated, this, &MainWindow::iconActivated);
    connect(mShowWindow,SIGNAL(triggered()),this,SLOT(on_showWindowAction()));
    connect(mAdvConf, &QAction::triggered, this, &MainWindow::actionTriggerSlots);

    //checkSingleAndShowTrayicon();
    //trayIcon->setVisible(true);

    objKyDBus = new KylinDBus(this);
    objKyDBus->initConnectionInfo();
    connect(objKyDBus, SIGNAL(toGetWifiListFinished(QStringList)), this, SLOT(loadWifiListDone(QStringList)));

    objNetSpeed = new NetworkSpeed();

    QApplication::setQuitOnLastWindowClosed(false);
    this->confForm = new ConfForm();

    this->ksnm = new KSimpleNM();
    connect(ksnm, SIGNAL(getLanListFinished(QStringList)), this, SLOT(getLanListDone(QStringList)));
    connect(ksnm, SIGNAL(getWifiListFinished(QStringList)), this, SLOT(getWifiListDone(QStringList)));
    connect(ksnm, SIGNAL(getConnListFinished(QStringList)), this, SLOT(getConnListDone(QStringList)));
    connect(ksnm, SIGNAL(requestRevalueUpdateWifi()), this, SLOT(onRequestRevalueUpdateWifi()));

    loading = new LoadingDiv(this);
    loading->move(40,0);
    connect(loading, SIGNAL(toStopLoading() ), this, SLOT(on_checkOverTime() ));

    checkIsWirelessDevicePluggedIn(); //检测无线网卡是否插入
    initLanSlistAndGetReconnectNetList(); //初始化有线网列表
    initNetwork(); //初始化网络
    initTimer(); //初始化定时器
    initActNetDNS();//初始化已连接网络的DNS
    getSystemFontFamily();//建立GSetting监听系统字体

    connect(ui->btnNetList, &QPushButton::clicked, this, &MainWindow::onBtnNetListClicked);
    connect(btnWireless, &SwitchButton::clicked,this, &MainWindow::onBtnWifiClicked);

    ui->btnNetList->setAttribute(Qt::WA_Hover,true);
    ui->btnNetList->installEventFilter(this);
    ui->btnWifiList->setAttribute(Qt::WA_Hover,true);
    ui->btnWifiList->installEventFilter(this);

    hasWifiConnected = false;
    numberForWifiScan = 0;

    //检查有线网络的个数是否为0,如果是0，则新建一个有线网络
    checkIfWiredNetExist();
}

MainWindow::~MainWindow()
{
    trayIcon->deleteLater();
    trayIconMenu->deleteLater();
    delete ui;
}

void MainWindow::checkSingleAndShowTrayicon()
{
    int fd = 0;
    try {
        QStringList homePath = QStandardPaths::standardLocations(QStandardPaths::HomeLocation);
        QString lockPath = QString(homePath.at(0) + "/.config/kylin-nm-lock-%1.lock").arg(getenv("DISPLAY")).toUtf8().data();
        fd = open(lockPath.toUtf8().data(), O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
        if (fd < 0) {
            throw -1;
        }
    } catch(...) {
        fd = open("/tmp/kylin-nm-lock", O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
        if (fd < 0) {
            exit(0);
        }
    }


    if (lockf(fd, F_TLOCK, 0)) {
        //syslog(LOG_ERR, "Can't lock single file, kylin-network-manager is already running!");
        //qDebug()<<"Can't lock single file, kylin-network-manager is already running!";
        //exit(0);
    } else {
        trayIcon->setVisible(true);
    }
}

void MainWindow::justShowTrayIcon()
{
    trayIcon->setVisible(true);
}

bool MainWindow::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == ui->btnNetList) {
        if (event->type() == QEvent::HoverEnter) {
            if (!is_btnLanList_clicked) {
                ui->lbNetListBG->setStyleSheet(btnBgHoverQss);
            }
            return true;
        } else if(event->type() == QEvent::HoverLeave) {
            if (!is_btnLanList_clicked) {
                ui->lbNetListBG->setStyleSheet(btnBgLeaveQss);
            }
            return true;
        }
    }

    if (obj == ui->btnWifiList) {
        if (event->type() == QEvent::HoverEnter) {
            if (!is_btnWifiList_clicked) {
                ui->lbWifiListBG->setStyleSheet(btnBgHoverQss);
            }
            return true;
        } else if(event->type() == QEvent::HoverLeave) {
            if (!is_btnWifiList_clicked) {
                ui->lbWifiListBG->setStyleSheet(btnBgLeaveQss);
            }
            return true;
        }
    }

    return QWidget::eventFilter(obj,event);
}

///////////////////////////////////////////////////////////////////////////////
// 初始化控件、网络、定时器

// 初始化界面各控件
void MainWindow::editQssString()
{
    btnOffQss = "QLabel{min-width: 37px; min-height: 37px;max-width:37px; max-height: 37px;border-radius: 4px; background-color:rgba(255,255,255,0)}";
    btnOnQss = "QLabel{min-width: 37px; min-height: 37px;max-width:37px; max-height: 37px;border-radius: 4px; background-color:rgba(61,107,229,1)}";
    btnBgOffQss = "QLabel{min-width: 48px; min-height: 22px;max-width:48px; max-height: 22px;border-radius: 10px; background-color:rgba(255,255,255,0.2)}";
    btnBgOnQss = "QLabel{min-width: 48px; min-height: 22px;max-width:48px; max-height: 22px;border-radius: 10px; background-color:rgba(61,107,229,1);}";
    btnBgHoverQss = "QLabel{border-radius: 4px; background-color:rgba(156,156,156,0.3)}";
    btnBgLeaveQss = "QLabel{border-radius: 4px; background-color:rgba(255,255,255,0)}";
    leftBtnQss = "QPushButton{border:0px;border-radius:4px;background-color:rgba(255,255,255,0);}"
                 "QPushButton:Hover{border:0px solid rgba(255,255,255,0.2);border-radius:4px;background-color:rgba(156,156,156,0.3);}"
                 "QPushButton:Pressed{border-radius:4px;background-color:rgba(156,156,156,0.3);}";
    funcBtnQss = "QPushButton{border:0px;border-radius:4px;background-color:rgba(255,255,255,0);color:rgba(107,142,235,0.97);font-size:14px;}"
                 "QPushButton:Hover{border:0px;border-radius:4px;background-color:rgba(255,255,255,0);color:rgba(151,175,241,0.97);font-size:14px;}"
                 "QPushButton:Pressed{border-radius:4px;background-color:rgba(255,255,255,0);color:rgba(61,107,229,0.97);font-size:14px;}";
}

void MainWindow::createTopLanUI()
{
    topLanListWidget = new QWidget(ui->centralWidget);
    topLanListWidget->move(W_LEFT_AREA, Y_TOP_ITEM);
    topLanListWidget->resize(W_TOP_LIST_WIDGET, H_NORMAL_ITEM + H_GAP_UP + X_ITEM);
    /*顶部的一个item*/
    lbTopLanList = new QLabel(topLanListWidget);
    lbTopLanList->setText(tr("Ethernet Networks"));//"可用网络列表"
    lbTopLanList->resize(W_MIDDLE_WORD, H_MIDDLE_WORD);
    lbTopLanList->move(X_MIDDLE_WORD, H_NORMAL_ITEM + H_GAP_UP);
    QFont fontTopLanList( "Noto Sans CJK SC", 14);
    lbTopLanList->setFont(fontTopLanList);
    lbTopLanList->show();
    /*新建有线网按钮*/
    btnCreateNet = new QPushButton(topLanListWidget);
    btnCreateNet->resize(W_BTN_FUN, H_BTN_FUN);
    btnCreateNet->move(X_BTN_FUN, Y_BTN_FUN);
    btnCreateNet->setText(tr("New LAN"));//"新建网络"
    btnCreateNet->setStyleSheet(funcBtnQss);
    btnCreateNet->setFocusPolicy(Qt::NoFocus);
    btnCreateNet->show();
    connect(btnCreateNet,SIGNAL(clicked()),this,SLOT(onBtnCreateNetClicked()));
}

void MainWindow::createTopWifiUI()
{
    topWifiListWidget = new QWidget(ui->centralWidget);
    topWifiListWidget->move(W_LEFT_AREA, Y_TOP_ITEM);
    topWifiListWidget->resize(W_TOP_LIST_WIDGET, H_NORMAL_ITEM + H_GAP_UP + X_ITEM);
    /*顶部的一个item*/
    lbTopWifiList = new QLabel(topWifiListWidget);
    lbTopWifiList->setText(tr("Wifi Networks"));//"可用网络列表"
    lbTopWifiList->resize(W_MIDDLE_WORD, H_MIDDLE_WORD);
    lbTopWifiList->move(X_MIDDLE_WORD, H_NORMAL_ITEM + H_GAP_UP);
    QFont fontTopWifiList( "Noto Sans CJK SC", 14);
    lbTopWifiList->setFont(fontTopWifiList);
    lbTopWifiList->show();
    /*新建有线网按钮*/
    btnAddNet = new QPushButton(topWifiListWidget);
    btnAddNet->resize(W_BTN_FUN, H_BTN_FUN);
    btnAddNet->move(X_BTN_FUN, Y_BTN_FUN);
    btnAddNet->setText(tr("Hide WiFi"));//"加入网络"
    btnAddNet->setStyleSheet(funcBtnQss);
    btnAddNet->setFocusPolicy(Qt::NoFocus);
    btnAddNet->show();
    connect(btnAddNet,SIGNAL(clicked()),this,SLOT(onBtnAddNetClicked()));
}

void MainWindow::createOtherUI()
{
    lbLoadDown = new QLabel(ui->centralWidget);
    lbLoadDown->move(X_ITEM + 129, Y_TOP_ITEM + 32);
    lbLoadDown->resize(65, 20);
    lbLoadDownImg = new QLabel(ui->centralWidget);
    lbLoadDownImg->move(X_ITEM + 112, Y_TOP_ITEM + 35);
    lbLoadDownImg->resize(16, 16);

    lbLoadUp = new QLabel(ui->centralWidget);
    lbLoadUp->move(X_ITEM + 187, Y_TOP_ITEM + 32);
    lbLoadUp->resize(65, 20);
    lbLoadUpImg = new QLabel(ui->centralWidget);
    lbLoadUpImg->move(X_ITEM + 170, Y_TOP_ITEM + 35);
    lbLoadUpImg->resize(16, 16);

    lbLoadDownImg->setStyleSheet("QLabel{background-image:url(:/res/x/load-down.png);}");
    lbLoadUpImg->setStyleSheet("QLabel{background-image:url(:/res/x/load-up.png);}");

    lbNoItemTip = new QLabel(ui->centralWidget);
    lbNoItemTip->resize(W_NO_ITEM_TIP, H_NO_ITEM_TIP);
    lbNoItemTip->move(this->width()/2 - W_NO_ITEM_TIP/2 + W_LEFT_AREA/2, this->height()/2);
    lbNoItemTip->setStyleSheet("QLabel{border:none;background:transparent;}");
    lbNoItemTip->setText(tr("No usable network in the list"));//列表暂无可连接网络
    lbNoItemTip->setAlignment(Qt::AlignCenter);
    lbNoItemTip->hide();
}

void MainWindow::createListAreaUI()
{
    scrollAreal = new QScrollArea(ui->centralWidget);
    scrollAreal->move(W_LEFT_AREA, Y_TOP_ITEM + H_NORMAL_ITEM + H_GAP_UP + X_ITEM + H_GAP_DOWN);
    scrollAreal->resize(W_SCROLL_AREA, H_SCROLL_AREA);
    scrollAreal->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollAreal->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    scrollAreaw = new QScrollArea(ui->centralWidget);
    scrollAreaw->move(W_LEFT_AREA, Y_TOP_ITEM + H_NORMAL_ITEM + H_GAP_UP + X_ITEM + H_GAP_DOWN);
    scrollAreaw->resize(W_SCROLL_AREA, H_SCROLL_AREA);
    scrollAreaw->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollAreaw->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    lanListWidget = new QWidget(scrollAreal);
    wifiListWidget = new QWidget(scrollAreaw);
    lbLanList = new QLabel(lanListWidget);
    lbWifiList = new QLabel(wifiListWidget);

    ui->lbNetwork->setStyleSheet("QLabel{font-size:20px;}");
    ui->lbNetwork->show();

    topLanListWidget->setStyleSheet("QWidget{border:none;}");
    topLanListWidget->setStyleSheet("background-color:transparent;");

    topWifiListWidget->setStyleSheet("QWidget{border:none;}");
    topWifiListWidget->setStyleSheet("background-color:transparent;");

    lbLoadUp->setStyleSheet("QLabel{font-size:14px;}");
    lbLoadDown->setStyleSheet("QLabel{font-size:14px;}");
    lbLoadUp->setText("0KB/s");
    lbLoadDown->setText("0KB/s.");
    lbLoadUp->hide();
    lbLoadDown->hide();
    lbLoadUpImg->hide();
    lbLoadDownImg->hide();

    this->on_setNetSpeed();

    scrollAreal->setStyleSheet("QScrollArea{border:none;}");
    scrollAreal->viewport()->setStyleSheet("background-color:transparent;");
    //scrollAreal->verticalScrollBar()->setStyleSheet(scrollBarQss);////

    scrollAreaw->setStyleSheet("QScrollArea{border:none;}");
    scrollAreaw->viewport()->setStyleSheet("background-color:transparent;");
    //scrollAreaw->verticalScrollBar()->setStyleSheet(scrollBarQss);////
}

void MainWindow::createLeftAreaUI()
{
    btnWireless = new SwitchButton(this);
    btnWireless->setStyleSheet("SwitchButton{border:none;background-color:rgba(255,255,255,0.12);}");
    ui->btnNetList->setFocusPolicy(Qt::NoFocus);
    QString txtEthernet(tr("Ethernet"));
    ui->btnNetList->setToolTip(txtEthernet);
    ui->lbNetListBG->setStyleSheet(btnOffQss);
    //设置PushButton背景透明
    QPalette paletteLan = ui->btnNetListImg->palette();
    paletteLan.setColor(QPalette::Highlight, Qt::transparent);
    paletteLan.setBrush(QPalette::Button, QBrush(QColor(1,1,1,0)));
    ui->btnNetListImg->setPalette(paletteLan);
    //添加PushButton的svg图片
    ui->btnNetListImg->setIcon(QIcon(":/res/x/net-list-bg.svg"));
    ui->btnNetListImg->setProperty("useIconHighlightEffect", true);
    ui->btnNetListImg->setProperty("iconHighlightEffectMode", true);

    ui->btnWifiList->setFocusPolicy(Qt::NoFocus);
    QString txtWifi("Wifi");
    ui->btnWifiList->setToolTip(txtWifi);
    ui->lbWifiListBG->setStyleSheet(btnOffQss);
    //设置PushButton背景透明
    QPalette paletteWifi = ui->btnWifiListImg->palette();
    paletteWifi.setColor(QPalette::Highlight, Qt::transparent);
    paletteWifi.setBrush(QPalette::Button, QBrush(QColor(1,1,1,0)));
    ui->btnWifiListImg->setPalette(paletteWifi);
    //添加PushButton的svg图片
    ui->btnWifiListImg->setIcon(QIcon(":/res/x/wifi-list-bg.svg"));
    ui->btnWifiListImg->setProperty("useIconHighlightEffect", true);
    ui->btnWifiListImg->setProperty("iconHighlightEffectMode", true);

    ui->btnNet->hide();

    btnWireless->setGeometry(412,20,50,24);

    ui->btnHotspot->setStyleSheet(leftBtnQss);
    ui->btnHotspot->setFocusPolicy(Qt::NoFocus);
    QString txtHotSpot(tr("HotSpot"));
    ui->btnHotspot->setToolTip(txtHotSpot);
    ui->btnHotspot->hide();
    ui->lbHotImg->hide();
    ui->lbHotImg->setStyleSheet("QLabel{background-image:url(:/res/x/hot-spot-off.svg);}");
    ui->lbHotBG->hide();
    ui->lbHotBG->setStyleSheet(btnOffQss);

    ui->btnFlyMode->setStyleSheet(leftBtnQss);
    ui->btnFlyMode->setFocusPolicy(Qt::NoFocus);
    QString txtFlyMode(tr("FlyMode"));
    ui->btnFlyMode->setToolTip(txtFlyMode);
    ui->btnFlyMode->hide();
    ui->lbFlyImg->hide();
    ui->lbFlyImg->setStyleSheet("QLabel{background-image:url(:/res/x/fly-mode-off.svg);}");
    ui->lbFlyBG->hide();
    ui->lbFlyBG->setStyleSheet(btnOffQss);

    ui->btnAdvConf->setStyleSheet(leftBtnQss);
    ui->btnAdvConf->setFocusPolicy(Qt::NoFocus);
    QString txtAdvanced(tr("Advanced"));
    ui->btnAdvConf->setToolTip(txtAdvanced);
    //设置PushButton背景透明
    QPalette paletteConf = ui->btnConfImg->palette();
    paletteConf.setColor(QPalette::Highlight, Qt::transparent);
    paletteConf.setBrush(QPalette::Button, QBrush(QColor(1,1,1,0)));
    ui->btnConfImg->setPalette(paletteConf);
    //添加PushButton的svg图片
    ui->btnConfImg->setIcon(QIcon(":/res/x/control.svg"));
    ui->btnConfImg->setProperty("useIconHighlightEffect", true);
    ui->btnConfImg->setProperty("iconHighlightEffectMode", true);
}

// 初始化有线网列表,初始化可回连wifi列表
void MainWindow::initLanSlistAndGetReconnectNetList()
{
    canReconnectWifiList.clear();
    const int BUF_SIZE = 1024;
    char buf[BUF_SIZE];

    FILE * p_file = NULL;

    //p_file = popen("export LANG='en_US.UTF-8';export LANGUAGE='en_US';nmcli -f type,device,name connection show", "r");
    //nmcli -f type,uuid,name connection show
    p_file = popen("export LANG='en_US.UTF-8';export LANGUAGE='en_US';nmcli -f type,uuid,name connection show", "r");
    if (!p_file) {
        syslog(LOG_ERR, "Error occurred when popen cmd 'nmcli connection show'");
        qDebug()<<"Error occurred when popen cmd 'nmcli connection show";
    }

    int trimNamePos = 0;
    while (fgets(buf, BUF_SIZE, p_file) != NULL) {
        QString strSlist = "";
        QString line(buf);
        strSlist = line.trimmed();
        if (strSlist.indexOf("UUID") != -1 || strSlist.indexOf("NAME") != -1) {
            trimNamePos = strSlist.indexOf("NAME");
            oldLanSlist.append(strSlist);
        }
        if (strSlist.indexOf("802-3-ethernet") != -1 || strSlist.indexOf("ethernet") != -1) {
            oldLanSlist.append(strSlist);
        }
        if (strSlist.indexOf("802-11-wireless") != -1 || strSlist.indexOf("wifi") != -1) {
            if (trimNamePos != 0) {
                QString mywifiname = strSlist.mid(trimNamePos).trimmed();//这里没有考虑wifi为空格的情况
                if (!canReconnectWifiList.contains(mywifiname)) {
                    canReconnectWifiList.append(mywifiname);
                }
            }
        }
    }

    pclose(p_file);
}

// 初始化网络
void MainWindow::initNetwork()
{
    BackThread *bt = new BackThread();
    IFace *iface = bt->execGetIface();

    wcardname = iface->wname;
    lwname = iface->wname;
    lcardname = iface->lname;
    llname = iface->lname;
    confForm->lcard = lcardname;
    confForm->wcard = wcardname;

    mwBandWidth = bt->execChkLanWidth(lcardname);

    // 开关状态
    qDebug()<<"===";
    qDebug()<<"state of network: '0' is connected, '1' is disconnected, '2' is net device switch off";
    syslog(LOG_DEBUG, "state of network: '0' is connected, '1' is disconnected, '2' is net device switch off");
    qDebug()<<"current network state:  lan state ="<<iface->lstate<<",  wifi state ="<<iface->wstate ;
    syslog(LOG_DEBUG, "current network state:  wired state =%d,  wifi state =%d", iface->lstate, iface->wstate);
    qDebug()<<"===";

    if (iface->wstate == 0 || iface->wstate == 1) {
        btnWireless->setSwitchStatus(true);
    } else {
        btnWireless->setSwitchStatus(false);
    }

    // 初始化网络列表
    if (iface->wstate != 2) {
        if (iface->wstate == 0) {
            connWifiDone(3);
        } else {
            if (iface->lstate == 0) {
                connLanDone(3);
            }
        }
        is_init_wifi_list = 1;
        on_btnWifiList_clicked();

        ui->btnNetList->setStyleSheet("QPushButton{border:0px solid rgba(255,255,255,0);background-color:rgba(255,255,255,0);}");
        ui->btnWifiList->setStyleSheet("QPushButton{border:none;}");
    } else {
        objKyDBus->setWifiSwitchState(false); //通知控制面板wifi未开启
        if (iface->lstate != 2) {
            if (iface->lstate == 0) {
                connLanDone(3);
            }
            onBtnNetListClicked();

            ui->btnNetList->setStyleSheet("QPushButton{border:0px solid rgba(255,255,255,0);background-color:rgba(255,255,255,0);}");
            ui->btnWifiList->setStyleSheet("QPushButton{border:none;}");
        } else {
            BackThread *m_bt = new BackThread();
            IFace *m_iface = m_bt->execGetIface();

            //写成信号监听自动执行，避免阻塞主线程
           disconnect_time = 1;
           connect (m_bt, &BackThread::btFinish, this, [ = ](){
               disconnect_time ++;
               if (disconnect_time <= 3) {
                   m_bt->disConnLanOrWifi("ethernet");
               } else {
                   m_bt->disconnect();
               }
           });
            m_bt->disConnLanOrWifi("ethernet");

            delete m_iface;
            m_bt->deleteLater();

            char *chr = "nmcli networking on";
            Utils::m_system(chr);

            onBtnNetListClicked();

            ui->btnNetList->setStyleSheet("QPushButton{border:0px solid rgba(255,255,255,0);background-color:rgba(255,255,255,0);}");
            ui->btnWifiList->setStyleSheet("QPushButton{border:none;}");
        }
    }
    ksnm->execGetConnList();

    QString actWifiUuid = objKyDBus->getActiveWifiUuid();
    objKyDBus->getConnectNetIp(actWifiUuid);
    objKyDBus->getWifiIp(actWifiUuid);
    confForm->actWifiIpv6Addr = objKyDBus->dbusActiveWifiIpv6;
}

// 初始化定时器
void MainWindow::initTimer()
{
    //应用启动后，需要连接可连接的网络
    QTimer::singleShot(1*1000, this, SLOT(toReconnectWifi() ));

    //循环检测wifi列表的变化，可用于更新wifi列表
    checkWifiListChanged = new QTimer(this);
    checkWifiListChanged->setTimerType(Qt::PreciseTimer);
    QObject::connect(checkWifiListChanged, SIGNAL(timeout()), this, SLOT(onRequestScanAccesspoint()));
    checkWifiListChanged->start(10*1000);

    //网线插入时定时执行
    wiredCableUpTimer = new QTimer(this);
    wiredCableUpTimer->setTimerType(Qt::PreciseTimer);
    QObject::connect(wiredCableUpTimer, SIGNAL(timeout()), this, SLOT(onCarrierUpHandle()));

    //网线拔出时定时执行
    wiredCableDownTimer = new QTimer(this);
    wiredCableDownTimer->setTimerType(Qt::PreciseTimer);
    QObject::connect(wiredCableDownTimer, SIGNAL(timeout()), this, SLOT(onCarrierDownHandle()));

    //定时处理异常网络，即当点击Lan列表按钮时，若lstate=2，但任然有有线网连接的情况
    deleteLanTimer = new QTimer(this);
    deleteLanTimer->setTimerType(Qt::PreciseTimer);
    QObject::connect(deleteLanTimer, SIGNAL(timeout()), this, SLOT(onDeleteLan()));

    //定时获取网速
    setNetSpeed = new QTimer(this);
    setNetSpeed->setTimerType(Qt::PreciseTimer);
    QObject::connect(setNetSpeed, SIGNAL(timeout()), this, SLOT(on_setNetSpeed()));
    setNetSpeed->start(1000);
}

//初始化已经连接网络的DNS
void MainWindow::initActNetDNS()
{
    QList<QString> currConnLanSsidUuidState =objKyDBus->getAtiveLanSsidUuidState();

    if (currConnLanSsidUuidState.size() > 0) {
        oldActLanName = currConnLanSsidUuidState.at(0);
        objKyDBus->getLanIpDNS(currConnLanSsidUuidState.at(1), true);
        oldDbusActLanDNS = objKyDBus->dbusActLanDNS;
    }
}

///////////////////////////////////////////////////////////////////////////////
// 任务栏托盘管理、托盘图标处理

void MainWindow::createTrayIcon()
{
    trayIcon = new QSystemTrayIcon();
    trayIcon->setToolTip(QString(tr("kylin-nm")));

    trayIconMenu = new QMenu();

    mShowWindow = new QAction(tr("Show MainWindow"),this);
    mAdvConf = new QAction(tr("Advanced"),this);
    mAdvConf->setIcon(QIcon::fromTheme("document-page-setup-symbolic", QIcon(":/res/x/setup.png")) );

    trayIconMenu->addAction(mShowWindow);
    trayIconMenu->addAction(mAdvConf);
    trayIcon->setContextMenu(trayIconMenu);

    // 初始化托盘所有Icon
    iconLanOnline = QIcon::fromTheme("network-wired-symbolic");
    iconLanOffline = QIcon::fromTheme("network-wired-offline-symbolic");
    iconWifiFull = QIcon::fromTheme("network-wireless-signal-excellent-symbolic");
    iconWifiHigh = QIcon::fromTheme("network-wireless-signal-good-symbolic");
    iconWifiMedium = QIcon::fromTheme("network-wireless-signal-ok");
    iconWifiLow = QIcon::fromTheme("network-wireless-signal-low");

    loadIcons.append(QIcon::fromTheme("kylin-network-1"));
    loadIcons.append(QIcon::fromTheme("kylin-network-2"));
    loadIcons.append(QIcon::fromTheme("kylin-network-3"));
    loadIcons.append(QIcon::fromTheme("kylin-network-4"));
    loadIcons.append(QIcon::fromTheme("kylin-network-5"));
    loadIcons.append(QIcon::fromTheme("kylin-network-6"));
    loadIcons.append(QIcon::fromTheme("kylin-network-7"));
    loadIcons.append(QIcon::fromTheme("kylin-network-8"));
    loadIcons.append(QIcon::fromTheme("kylin-network-9"));
    loadIcons.append(QIcon::fromTheme("kylin-network-10"));
    loadIcons.append(QIcon::fromTheme("kylin-network-11"));
    loadIcons.append(QIcon::fromTheme("kylin-network-12"));

    iconTimer = new QTimer(this);
    connect(iconTimer, SIGNAL(timeout()), this, SLOT(iconStep()));

    setTrayIcon(iconLanOnline);
}

void MainWindow::iconStep()
{
    if (currentIconIndex > 11) {
        currentIconIndex = 0;
    }
    setTrayIcon(loadIcons.at(currentIconIndex));
    currentIconIndex ++;
}

void MainWindow::setTrayIcon(QIcon icon)
{
    trayIcon->setIcon(icon);
}

void MainWindow::setTrayIconOfWifi(int signal){
    if (signal > 75) {
        setTrayIcon(iconWifiFull);
    } else if(signal > 55 && signal <= 75) {
        setTrayIcon(iconWifiHigh);
    } else if(signal > 35 && signal <= 55) {
        setTrayIcon(iconWifiMedium);
    } else if( signal <= 35) {
        setTrayIcon(iconWifiLow);
    }
}

void MainWindow::setTrayLoading(bool isLoading)
{
    if (isLoading) {
        currentIconIndex = 0;
        iconTimer->start(60);
    } else {
        iconTimer->stop();
    }
}

void MainWindow::iconActivated(QSystemTrayIcon::ActivationReason reason)
{
    switch (reason) {
    case QSystemTrayIcon::Trigger:
    case QSystemTrayIcon::MiddleClick:
        handleIconClicked();

        if (this->isHidden()) {
            this->showNormal();
            this->raise();
            this->activateWindow();
            if (is_btnLanList_clicked == 1&& is_stop_check_net_state==0) {
                onBtnNetListClicked(0);
            }
            if (!is_init_wifi_list && !is_connect_hide_wifi && is_stop_check_net_state==0) {
                is_stop_check_net_state = 1;
                if (is_btnWifiList_clicked == 1) {
                    BackThread *loop_bt = new BackThread();
                    IFace *loop_iface = loop_bt->execGetIface();

                    if (loop_iface->wstate != 2) {
                        //current_wifi_list_state = UPDATE_WIFI_LIST;
                        //checkIfConnectedWifiExist();
                        //this->ksnm->execGetWifiList(); //更新wifi列表
                        this->on_btnWifiList_clicked(); //加载wifi列表
                    }

                    delete loop_iface;
                    loop_bt->deleteLater();
                }
                is_stop_check_net_state = 0;
            }
        } else {
            this->hide();
            numberForWifiScan = 0;
        }
        break;
    case QSystemTrayIcon::DoubleClick:
        //this->hide();
        break;
    case QSystemTrayIcon::Context:
        //右键点击托盘图标弹出菜单
//        showTrayIconMenu();
        break;
    default:
        break;
    }
}

void MainWindow::handleIconClicked()
{
    QRect availableGeometry = qApp->primaryScreen()->availableGeometry();
    QRect screenGeometry = qApp->primaryScreen()->geometry();

    QDesktopWidget* desktopWidget = QApplication::desktop();
    QRect deskMainRect = desktopWidget->availableGeometry(0);//获取可用桌面大小
    QRect screenMainRect = desktopWidget->screenGeometry(0);//获取设备屏幕大小
    QRect deskDupRect = desktopWidget->availableGeometry(1);//获取可用桌面大小
    QRect screenDupRect = desktopWidget->screenGeometry(1);//获取设备屏幕大小

    int n = objKyDBus->getTaskBarPos("position");
    int m = objKyDBus->getTaskBarHeight("height");
    int d = 4; //窗口边沿到任务栏距离

    if (isHuaWeiPC) {
        //qDebug() << "-------------------------------------> HUAWEI PC";
        //qDebug("primary screen  changed, geometry is  x=%d, y=%d, windth=%d, height=%d", m_priX, m_priY, m_priWid, m_priHei);
        if (n == 0) {
            //任务栏在下侧
            this->move(m_priX + m_priWid - this->width() - d, m_priY + m_priHei - this->height() - m - d);
        } else if(n == 1) {
            //任务栏在上侧
            this->move(m_priX + m_priWid - this->width() - d, m_priY + m + d);
        } else if (n == 2) {
            //任务栏在左侧
            this->move(m_priX + m + d, m_priY + m_priHei - this->height() - d);
        } else if (n == 3) {
            //任务栏在右侧
            this->move(m_priX + m_priWid - this->width() - m - d, m_priY + m_priHei - this->height() - d);
        }
    } else {
        if (screenGeometry.width() == availableGeometry.width() && screenGeometry.height() == availableGeometry.height()) {
            if (n == 0) {
                //任务栏在下侧
                this->move(availableGeometry.x() + availableGeometry.width() - this->width() - d, screenMainRect.y() + availableGeometry.height() - this->height() - m - d);
            } else if(n == 1) {
                //任务栏在上侧
                this->move(availableGeometry.x() + availableGeometry.width() - this->width() - d, screenMainRect.y() + screenGeometry.height() - availableGeometry.height() + m + d);
            } else if (n == 2) {
                //任务栏在左侧
                if (screenGeometry.x() == 0) {//主屏在左侧
                    this->move(m + d, screenMainRect.y() + screenMainRect.height() - this->height() - d);
                } else {//主屏在右侧
                    this->move(screenMainRect.x() + m + d, screenMainRect.y() + screenMainRect.height() - this->height() - d);
                }
            } else if (n == 3) {
                //任务栏在右侧
                if (screenGeometry.x() == 0) {//主屏在左侧
                    this->move(screenMainRect.width() - this->width() - m - d, screenMainRect.y() + screenMainRect.height() - this->height() - d);
                } else {//主屏在右侧
                    this->move(screenMainRect.x() + screenMainRect.width() - this->width() - m - d, screenMainRect.y() + screenMainRect.height() - this->height() - d);
                }
            }
        } else if(screenGeometry.width() == availableGeometry.width() ) {
            if (trayIcon->geometry().y() > availableGeometry.height()/2) {
                //任务栏在下侧
                if (screenGeometry.height() - 20 < availableGeometry.height()) {
                    //任务栏隐藏
                    this->move(availableGeometry.x() + availableGeometry.width() - this->width() - d, screenMainRect.y() + availableGeometry.height() - this->height() - m );
                } else {
                    //任务栏不隐藏
                    this->move(availableGeometry.x() + availableGeometry.width() - this->width() - d, screenMainRect.y() + availableGeometry.height() - this->height() - d);
                }
            } else {
                //任务栏在上侧
                if (screenGeometry.height() - 20 < availableGeometry.height()) {
                    //任务栏隐藏
                    this->move(availableGeometry.x() + availableGeometry.width() - this->width() - d, screenMainRect.y() + screenGeometry.height() - availableGeometry.height() + m );
                } else {
                    //任务栏不隐藏
                    this->move(availableGeometry.x() + availableGeometry.width() - this->width() - d, screenMainRect.y() + screenGeometry.height() - availableGeometry.height() + d);
                }
            }
        } else if (screenGeometry.height() == availableGeometry.height()) {
            if (trayIcon->geometry().x() > availableGeometry.width()/2) {
                //任务栏在右侧
                if (screenGeometry.width() - 20 < availableGeometry.width()) {
                    //任务栏隐藏
                    this->move(availableGeometry.x() + availableGeometry.width() - this->width() - m, screenMainRect.y() + screenGeometry.height() - this->height() - d - 3);
                } else {
                    //任务栏不隐藏
                    this->move(availableGeometry.x() + availableGeometry.width() - this->width() - d, screenMainRect.y() + screenGeometry.height() - this->height() - d - 3);
                }
            } else {
                //任务栏在左侧
                if (screenGeometry.width() - 20 < availableGeometry.width()) {
                    //任务栏隐藏
                    this->move(screenGeometry.width() - availableGeometry.width() + m, screenMainRect.y() + screenGeometry.height() - this->height() - d - 3);
                } else {
                    //任务栏不隐藏
                    this->move(screenGeometry.width() - availableGeometry.width() + d, screenMainRect.y() + screenGeometry.height() - this->height() - d - 3);
                }
            }
        }
    }
}

void MainWindow::showTrayIconMenu()
{
    QRect availableGeometry = qApp->primaryScreen()->availableGeometry();
    QRect screenGeometry = qApp->primaryScreen()->geometry();

    QDesktopWidget* desktopWidget = QApplication::desktop();
    QRect screenMainRect = desktopWidget->screenGeometry(0);//获取设备屏幕大小
    QRect screenDupRect = desktopWidget->screenGeometry(1);//获取设备屏幕大小

    QPoint cursorPoint =  QCursor::pos();//返回相对显示器的全局坐标
    int cursor_x = cursorPoint.x();
    int cursor_y = cursorPoint.y();

    int n = objKyDBus->getTaskBarPos("position");
    int m = objKyDBus->getTaskBarHeight("height");
    int d = 0; //窗口边沿到任务栏距离
    int s = 80; //窗口边沿到屏幕边沿距离

    if (screenGeometry.width() == availableGeometry.width() && screenGeometry.height() == availableGeometry.height()) {
        if (n == 0) { //任务栏在下侧
            trayIconMenu->move(availableGeometry.x() + cursor_x - trayIconMenu->width()/2, screenMainRect.y() + availableGeometry.height() - trayIconMenu->height() - m - d);
        } else if(n == 1) { //任务栏在上侧
            trayIconMenu->move(availableGeometry.x() + cursor_x - trayIconMenu->width()/2, screenMainRect.y() + screenGeometry.height() - availableGeometry.height() + m + d);
        } else if (n == 2) { //任务栏在左侧
            trayIconMenu->move(m + d, cursor_y - trayIconMenu->height()/2);
        } else if (n == 3) { //任务栏在右侧
            trayIconMenu->move(screenMainRect.width() - trayIconMenu->width() - m - d, cursor_y - trayIconMenu->height()/2);
        }
    } else if(screenGeometry.width() == availableGeometry.width() ) {
        if (trayIcon->geometry().y() > availableGeometry.height()/2) { //任务栏在下侧
            trayIconMenu->move(availableGeometry.x() + cursor_x - trayIconMenu->width()/2, screenMainRect.y() + availableGeometry.height() - trayIconMenu->height() - d);
        } else { //任务栏在上侧
            trayIconMenu->move(availableGeometry.x() + cursor_x - trayIconMenu->width()/2, screenMainRect.y() + screenGeometry.height() - availableGeometry.height() + d);
        }
    } else if (screenGeometry.height() == availableGeometry.height()) {
        if (trayIcon->geometry().x() > availableGeometry.width()/2) { //任务栏在右侧
            trayIconMenu->move(availableGeometry.x() + availableGeometry.width() - trayIconMenu->width() - d, cursor_y - trayIconMenu->height()/2);
        } else { //任务栏在左侧
            trayIconMenu->move(screenGeometry.width() - availableGeometry.width() + d, cursor_y - trayIconMenu->height()/2);
        }
    }
}

void MainWindow::on_showWindowAction()
{
    handleIconClicked();
    this->showNormal();
    this->raise();
    this->activateWindow();
}


///////////////////////////////////////////////////////////////////////////////
//加载动画,获取当前连接的网络和状态并设置图标

void MainWindow::startLoading()
{
    loading->startLoading();
    setTrayLoading(true);
}

void MainWindow::stopLoading()
{
    loading->stopLoading();
    setTrayLoading(false);
    getActiveInfoAndSetTrayIcon();
}

void MainWindow::on_checkOverTime()
{
    QString cmd = "kill -9 $(pidof nmcli)"; //杀掉当前正在进行的有关nmcli命令的进程
    int status = system(cmd.toUtf8().data());
    if (status != 0) {
        qDebug()<<"execute 'kill -9 $(pidof nmcli)' in function 'on_checkOverTime' failed";
        syslog(LOG_ERR, "execute 'kill -9 $(pidof nmcli)' in function 'on_checkOverTime' failed");
    }
    this->stopLoading(); //超时停止等待动画
    is_stop_check_net_state = 0;
}

void MainWindow::getActiveInfoAndSetTrayIcon()
{
    //bool hasNetConnecting =objKyDBus->checkNetworkConnectivity();
    //if (hasNetConnecting) {
    //    this->setTrayLoading(true);
    //    return;
    //}

    int wifiSignal = 0;
    if (activeWifiSignalLv == 0) {
        wifiSignal = objKyDBus->getActiveWifiSignal();
        if (wifiSignal != 0) {
            activeWifiSignalLv = wifiSignal;
        }
    }

    QString actLanName = "--";
    QString actWifiName = "--";

    activecon *act = kylin_network_get_activecon_info();
    int index = 0;
    while (act[index].con_name != NULL) {
        if (QString(act[index].type) == "ethernet" || QString(act[index].type) == "802-3-ethernet" || QString(act[index].type) == "bluetooth") {
            actLanName = QString(act[index].con_name);
        }
        if (QString(act[index].type) == "wifi" || QString(act[index].type) == "802-11-wireless") {
            actWifiName = QString(act[index].con_name);
        }
        index ++;
    }
    // 设置图标
    if (actLanName != "--") {
        QList<QString> lanstate = objKyDBus->getAtiveLanSsidUuidState();
        qDebug() << Q_FUNC_INFO << lanstate;
        if (lanstate[2] == "connected") {
            setTrayIcon(iconLanOnline);
        }
        else {
            setTrayLoading(true);
        }

    } else if (actWifiName != "--") {
        setTrayIconOfWifi(activeWifiSignalLv);
        emit this->actWifiSignalLvChanaged(activeWifiSignalLv);
    } else {
        setTrayIcon(iconLanOffline);
    }
}


///////////////////////////////////////////////////////////////////////////////
//网络设备管理

//网线插拔处理,由kylin-dbus-interface.cpp调用
void MainWindow::onPhysicalCarrierChanged(bool flag)
{
    this->startLoading();
    if (flag) {
        is_stop_check_net_state = 1;
        qDebug()<<"插入了有线网的网线";
        syslog(LOG_DEBUG,"wired physical cable is already plug in");
        wiredCableUpTimer->start(2000);
    } else {
        qDebug()<<"拔出了有线网的网线";
        syslog(LOG_DEBUG,"wired physical cable is already plug out");
        activeLanDisconn();
        BackThread *bt = new BackThread();
        IFace *iface = bt->execGetIface();
        if (iface->lstate != 0) {
            is_stop_check_net_state = 1;
            wiredCableDownTimer->start(2000);
        }
        delete iface;
        bt->deleteLater();
    }
}

void MainWindow::onCarrierUpHandle()
{
    wiredCableUpTimer->stop();

    //检查有线网络的个数是否为0,如果是0，则新建一个有线网络
    checkIfWiredNetExist();

    this->stopLoading();
    onBtnNetListClicked(1);
    is_stop_check_net_state = 0;
}

void MainWindow::onCarrierDownHandle()
{
    wiredCableDownTimer->stop();
    this->stopLoading();
    onBtnNetListClicked(0);
    is_stop_check_net_state = 0;
}

void MainWindow::onDeleteLan()
{
    deleteLanTimer->stop();
    BackThread *btn_bt = new BackThread();
    //写成信号监听自动执行，避免阻塞主线程
    disconnect_time = 1;
    connect (btn_bt, &BackThread::btFinish, this, [ = ](){
        disconnect_time ++;
        if (disconnect_time <= 3) {
            btn_bt->disConnLanOrWifi("ethernet");
        } else {
            btn_bt->disconnect();
        }
    });
    btn_bt->disConnLanOrWifi("ethernet");
    btn_bt->deleteLater();

    this->stopLoading();
    onBtnNetListClicked(0);
    is_stop_check_net_state = 0;
}

void MainWindow::checkIfWiredNetExist()
{
    if (objKyDBus->getWiredNetworkNumber() == 0) {
        objKyDBus->toCreateNewLan();
    }
}

//无线网卡插拔处理
void MainWindow::onNetworkDeviceAdded(QDBusObjectPath objPath)
{
    //仅处理无线网卡插入情况
    objKyDBus->isWirelessCardOn = false;
    objKyDBus->getObjectPath();

    if (objKyDBus->multiWirelessPaths.at(0).path() == objPath.path()) { //证明添加的是无线网卡
        is_wireless_adapter_ready = 0;
        if (objKyDBus->isWirelessCardOn) {
            syslog(LOG_DEBUG,"wireless device is already plug in");
            qDebug()<<"wireless device is already plug in";
            is_wireless_adapter_ready = 1;
            onBtnWifiClicked(4);
            objKyDBus->getWirelessCardName();
        }
    }
}

void MainWindow::onNetworkDeviceRemoved(QDBusObjectPath objPath)
{
    //仅处理无线网卡拔出情况
    if (objKyDBus->multiWirelessPaths.at(0).path() == objPath.path()) {
        objKyDBus->isWirelessCardOn = false;
        objKyDBus->getObjectPath(); //检查是不是还有无线网卡
        if (!objKyDBus->isWirelessCardOn) {
            syslog(LOG_DEBUG,"wireless device is already plug out");
            qDebug()<<"wireless device is already plug out";
            is_wireless_adapter_ready = 0;
            onBtnWifiClicked(5);
        } else {
            objKyDBus->getWirelessCardName();
            syslog(LOG_DEBUG,"wireless device is already plug out, but one more wireless exist");
            qDebug()<<"wireless device is already plug out, but one more wireless exist";
        }
    }
}

void MainWindow::checkIsWirelessDevicePluggedIn()
{
    //启动时判断是否有无线网卡
    //KylinDBus kDBus3;
    if (objKyDBus->isWirelessCardOn) {
        is_wireless_adapter_ready = 1;
    } else {
        is_wireless_adapter_ready = 0;
    }
}

void MainWindow::getLanBandWidth()
{
    BackThread *bt = new BackThread();
    IFace *iface = bt->execGetIface();

    lcardname = iface->lname;

    mwBandWidth = bt->execChkLanWidth(lcardname);
}

//检测网络设备状态
bool MainWindow::checkLanOn()
{
    BackThread *bt = new BackThread();
    IFace *iface = bt->execGetIface();
    bt->deleteLater();
    int state = iface->lstate;
    if (state == 2) {
        return false;
    } else {
        return true;
    }
}

bool MainWindow::checkWlOn()
{
    BackThread *bt = new BackThread();
    IFace *iface = bt->execGetIface();
    bt->deleteLater();
    int state = iface->wstate;
    if (state == 2) {
        return false;
    } else {
        return true;
    }
}


///////////////////////////////////////////////////////////////////////////////
//有线网与无线网按钮响应

void MainWindow::onBtnNetClicked()
{
    if (checkLanOn()) {
        QThread *t = new QThread();
        BackThread *bt = new BackThread();
        bt->moveToThread(t);
        connect(t, SIGNAL(finished()), t, SLOT(deleteLater()));
        connect(t, SIGNAL(started()), bt, SLOT(execDisNet()));
        connect(bt, SIGNAL(disNetDone()), this, SLOT(disNetDone()));
        connect(bt, SIGNAL(btFinish()), t, SLOT(quit()));
        t->start();

    } else {
        is_stop_check_net_state = 1;
        QThread *t = new QThread();
        BackThread *bt = new BackThread();
        bt->moveToThread(t);
        connect(t, SIGNAL(finished()), t, SLOT(deleteLater()));
        connect(t, SIGNAL(started()), bt, SLOT(execEnNet()));
        connect(bt, SIGNAL(enNetDone()), this, SLOT(enNetDone()));
        connect(bt, SIGNAL(btFinish()), t, SLOT(quit()));
        t->start();
    }

    this->startLoading();
}

void MainWindow::onBtnWifiClicked(int flag)
{
    qDebug()<<"Value of flag passed into function 'onBtnWifiClicked' is:  "<<flag;
    syslog(LOG_DEBUG, "Value of flag passed into function 'onBtnWifiClicked' is: %d", flag);

    if (is_wireless_adapter_ready == 1) {
        // flag: 0->UI点击关闭 1->UI点击打开 2->gsetting打开 3->gsetting关闭 4->网卡热插 5->网卡热拔
        // 当连接上无线网卡时才能打开wifi开关
        // 网络开关关闭时，点击Wifi开关时，程序先打开有线开关
        if (flag == 0 || flag == 1 || flag == 4 || flag == 5) {
            if (checkWlOn()) {
                if (flag != 4) { //以防第二张无线网卡插入时断网
                    is_stop_check_net_state = 1;
                    objKyDBus->setWifiSwitchState(false);
                    lbTopWifiList->hide();
                    btnAddNet->hide();

                    QThread *t = new QThread();
                    BackThread *bt = new BackThread();
                    bt->moveToThread(t);
                    btnWireless->setSwitchStatus(true);
                    connect(t, SIGNAL(finished()), t, SLOT(deleteLater()));
                    connect(t, SIGNAL(started()), bt, SLOT(execDisWifi()));
                    connect(bt, SIGNAL(disWifiDone()), this, SLOT(disWifiDone()));
                    connect(bt, SIGNAL(btFinish()), t, SLOT(quit()));
                    t->start();
                    this->startLoading();
                }
            } else {
                if (is_fly_mode_on == 0) {
                    //on_btnWifiList_clicked();
                    is_stop_check_net_state = 1;
                    objKyDBus->setWifiCardState(true);
                    objKyDBus->setWifiSwitchState(true);

                    QThread *t = new QThread();
                    BackThread *bt = new BackThread();
                    bt->moveToThread(t);
                    btnWireless->setSwitchStatus(true);
                    connect(t, SIGNAL(finished()), t, SLOT(deleteLater()));
                    connect(t, SIGNAL(started()), bt, SLOT(execEnWifi()));
                    connect(bt, SIGNAL(enWifiDone()), this, SLOT(enWifiDone()));
                    connect(bt, SIGNAL(btFinish()), t, SLOT(quit()));
                    t->start();
                    this->startLoading();
                }
            }
        } else if(flag == 2) {
            if (is_fly_mode_on == 0) {
                //on_btnWifiList_clicked();
                is_stop_check_net_state = 1;
                lbTopWifiList->show();
                btnAddNet->show();

                QThread *t = new QThread();
                BackThread *bt = new BackThread();
                btnWireless->setSwitchStatus(true);
                bt->moveToThread(t);
                connect(t, SIGNAL(finished()), t, SLOT(deleteLater()));
                connect(t, SIGNAL(started()), bt, SLOT(execEnWifi()));
                connect(bt, SIGNAL(enWifiDone()), this, SLOT(enWifiDone()));
                connect(bt, SIGNAL(btFinish()), t, SLOT(quit()));
                t->start();
                this->startLoading();
            }
        } else if(flag == 3) {
            is_stop_check_net_state = 1;
            lbTopWifiList->hide();
            btnAddNet->hide();

            QThread *t = new QThread();
            BackThread *bt = new BackThread();
            btnWireless->setSwitchStatus(true);
            bt->moveToThread(t);
            connect(t, SIGNAL(finished()), t, SLOT(deleteLater()));
            connect(t, SIGNAL(started()), bt, SLOT(execDisWifi()));
            connect(bt, SIGNAL(disWifiDone()), this, SLOT(disWifiDone()));
            connect(bt, SIGNAL(btFinish()), t, SLOT(quit()));
            t->start();
            this->startLoading();
        } else {
            qDebug()<<"receive an invalid value in function onBtnWifiClicked";
            syslog(LOG_DEBUG, "receive an invalid value in function onBtnWifiClicked");
        }
    } else {
        lbTopWifiList->hide();
        btnAddNet->hide();

        if (flag == 0) {
             objKyDBus->setWifiSwitchState(false);
             objKyDBus->setWifiCardState(false);
        }

        QString txt(tr("No wireless card detected")); //未检测到无线网卡
        objKyDBus->showDesktopNotify(txt);
        qDebug()<<"No wireless card detected";
        syslog(LOG_DEBUG, "No wireless card detected");

        disWifiStateKeep();
    }
}

void MainWindow::onBtnNetListClicked(int flag)
{
    this->is_btnLanList_clicked = 1;
    this->is_btnWifiList_clicked = 0;
    end_rcv_rates = 0;
    end_tx_rates = 0;

    ui->lbNetListBG->setStyleSheet(btnOnQss);
    ui->lbWifiListBG->setStyleSheet(btnOffQss);

    BackThread *bt = new BackThread();
    IFace *iface = bt->execGetIface();

    lbLoadDown->hide();
    lbLoadUp->hide();
    lbLoadDownImg->hide();
    lbLoadUpImg->hide();

    lbNoItemTip->hide();

    ui->lbNetwork->setText(tr("Ethernet"));
    btnWireless->hide();

    // 强行设置为打开
    if (flag == 1) {
        this->startLoading();
        this->ksnm->execGetLanList();
        this->scrollAreal->show();
        this->topLanListWidget->show();
        this->scrollAreaw->hide();
        this->topWifiListWidget->hide();
        return;
    }

    if (iface->lstate == 0 || iface->lstate == 1) {
        this->startLoading();
        this->ksnm->execGetLanList();
    } else if (iface->lstate == 3) {
        this->ksnm->isUseOldLanSlist = true;
        QStringList slistLan;
        slistLan.append("empty");
        getLanListDone(slistLan);
    } else {
        this->startLoading();
        this->ksnm->execGetLanList();
    }

    this->scrollAreal->show();
    this->topLanListWidget->show();
    this->scrollAreaw->hide();
    this->topWifiListWidget->hide();

    delete iface;
    bt->deleteLater();
}

void MainWindow::on_btnWifiList_clicked()
{
    is_stop_check_net_state = 1;
    current_wifi_list_state = LOAD_WIFI_LIST;

    this->is_btnWifiList_clicked = 1;
    this->is_btnLanList_clicked = 0;
    end_rcv_rates = 0;
    end_tx_rates = 0;

    if (this->is_btnLanList_clicked == 1) {
        return;
    }

    BackThread *bt = new BackThread();
    IFace *iface = bt->execGetIface();

    if (iface->wstate != 0) {
        lbLoadDown->hide();
        lbLoadUp->hide();
        lbLoadDownImg->hide();
        lbLoadUpImg->hide();
        hasWifiConnected = false;
    } else {
        hasWifiConnected = true;
    }

    ui->lbNetListBG->setStyleSheet(btnOffQss);
    ui->lbWifiListBG->setStyleSheet(btnOnQss);

    lbNoItemTip->hide();

    ui->lbNetwork->setText("Wifi");
    btnWireless->show();

    if (iface->wstate == 0 || iface->wstate == 1) {
        qDebug() << "debug: wifi开关在打开状态";
        btnWireless->setSwitchStatus(true);
        lbTopWifiList->show();
        btnAddNet->show();

        this->startLoading();
        //this->objKyDBus->toGetWifiList();
        this->ksnm->execGetWifiList(this->wcardname);
    } else if (iface->wstate == 3) {
        qDebug() << "debug: 连接中，正在配置wifi设备";

        this->ksnm->isUseOldWifiSlist = true;
        QStringList slistWifi;
        slistWifi.append("empty");
        getWifiListDone(slistWifi);

        btnWireless->setSwitchStatus(true);
        lbTopWifiList->show();
        btnAddNet->show();
        is_stop_check_net_state = 0;
    } else {
        qDebug()<<"debug: WiFi的开关已经关闭";
        btnWireless->setSwitchStatus(false);
        delete topWifiListWidget; //清空top列表
        createTopWifiUI(); //创建顶部无线网item
        lbTopWifiList->hide();
        btnAddNet->hide();

        // 清空wifi列表
        wifiListWidget = new QWidget(scrollAreaw);
        wifiListWidget->resize(W_LIST_WIDGET, H_WIFI_ITEM_BIG_EXTEND);
        scrollAreaw->setWidget(wifiListWidget);
        scrollAreaw->move(W_LEFT_AREA, Y_SCROLL_AREA);

        // 当前连接的wifi
        OneConnForm *ccf = new OneConnForm(topWifiListWidget, this, confForm, ksnm);
        ccf->setName(tr("Not connected"), "--", "--", "--");//"当前未连接任何 Wifi"
        ccf->setSignal("0", "--");
        ccf->setRate("0");
        ccf->setConnedString(1, tr("Disconnected"), "");//"未连接"
        ccf->isConnected = false;
        ccf->setTopItem(false);
        ccf->setAct(true);
        ccf->move(L_VERTICAL_LINE_TO_ITEM, 0);
        ccf->show();
        ccf->lbFreq->hide();

        this->lanListWidget->hide();
        this->wifiListWidget->show();

        getActiveInfoAndSetTrayIcon();
        is_stop_check_net_state = 0;
    }

    this->scrollAreal->hide();
    this->topLanListWidget->hide();
    this->scrollAreaw->show();
    this->topWifiListWidget->show();

    delete iface;
    bt->deleteLater();
}

void MainWindow::on_wifi_changed()
{
    QString actWifiUuid = objKyDBus->getActiveWifiUuid();
    objKyDBus->getConnectNetIp(actWifiUuid);
    objKyDBus->getWifiIp(actWifiUuid);
    confForm->actWifiIpv6Addr = objKyDBus->dbusActiveWifiIpv6;
    if (oldWifiIpv4Method == "") {
        oldWifiIpv4Method = objKyDBus-> dbusWifiIpv4Method;
    }
    if (objKyDBus->dbusWifiIpv4 != "" && objKyDBus->dbusActiveWifiIpv4 != "" && objKyDBus->dbusWifiIpv4 != objKyDBus->dbusActiveWifiIpv4 &&objKyDBus-> dbusWifiIpv4Method == "manual") {
        //在第三方nm-connection-editor进行新的IP配置后，重新连接网络
        oldWifiIpv4Method = "manual";
        qDebug()<<"Ipv4.address of current activated wifi is:"<<objKyDBus->dbusActiveWifiIpv4 << ". Real ipv4.address is:" << objKyDBus->dbusWifiIpv4;
        emit this->reConnectWifi(actWifiUuid);
    } else if (objKyDBus-> dbusWifiIpv4Method == "auto" && oldWifiIpv4Method == "manual") {
        oldWifiIpv4Method = "auto";
        qDebug()<<"Ipv4.method is set to auto.";
        emit this->reConnectWifi(actWifiUuid);
    } else if (!objKyDBus->dbusActiveWifiIpv6.isEmpty() && objKyDBus->dbusActiveWifiIpv6 != objKyDBus->dbusWifiIpv6 && objKyDBus->dbusWifiIpv6Method == "manual") {
        //在第三方nm-connection-editor或kylin-nm配置页进行新的IPV6配置后，重新连接网络
        emit this->reConnectWifi(actWifiUuid);
    }
}

/**
 * @brief MainWindow::onNewConnAdded 获取新的连接列表
 * @param type 0为有线，1为无线
 */
void MainWindow::onNewConnAdded(int type) {
    if (type == 1) {
        isAddedWifi = true;
    }
    this->ksnm->execGetConnList();
}

///////////////////////////////////////////////////////////////////////////////
//网络列表加载与更新

// 获取lan列表回调
void MainWindow::getLanListDone(QStringList slist)
{
    if (this->is_btnWifiList_clicked == 1) {
        return;
    }

    //要求使用上一次获取到的列表
    if (this->ksnm->isUseOldLanSlist) {
        slist = oldLanSlist;
        this->ksnm->isUseOldLanSlist = false;
    }

    //若slist为空，则也使用上一次获取到的列表
    if (slist.size() == 1 && slist.at(0) == "") {
        if (oldLanSlist.size() == 1 && oldLanSlist.at(0) == "") {
            return;
        } else {
            slist = oldLanSlist;
        }
    }

    delete topLanListWidget; // 清空top列表
    createTopLanUI(); //创建顶部有线网item

    // 清空lan列表
    lanListWidget = new QWidget(scrollAreal);
    lanListWidget->resize(W_LIST_WIDGET, H_NORMAL_ITEM + H_LAN_ITEM_EXTEND);
    scrollAreal->setWidget(lanListWidget);
    scrollAreal->move(W_LEFT_AREA, Y_SCROLL_AREA);

    // 获取当前连接有线网的SSID和UUID
    QList<QString> actLanSsidName;
    QList<QString> actLanUuidName;
    QList<QString> actLanStateName;
    QList<QString> currConnLanSsidUuidState =objKyDBus->getAtiveLanSsidUuidState();

    bool hasCurrentLanConnected = false;
    if (currConnLanSsidUuidState.contains("connected")) {
        hasCurrentLanConnected = true;
    }

    // 若当前没有任何一个有线网连接，设置有线列表顶部的item状态为未连接
    if (!hasCurrentLanConnected) {
        OneLancForm *ccf = new OneLancForm(topLanListWidget, this, confForm, ksnm);
        ccf->setName(tr("Not connected"), tr("Not connected"), "--", "--");//"当前未连接任何 以太网"
        ccf->setIcon(false);
        ccf->setConnedString(1, tr("Disconnected"), "");//"未连接"
        ccf->isConnected = false;
        ifLanConnected = false;
        lbLoadDown->hide();
        lbLoadUp->hide();
        lbLoadDownImg->hide();
        lbLoadUpImg->hide();
        ccf->setTopItem(false);//"当前未连接任何 以太网"
        ccf->setAct(true);
        ccf->move(L_VERTICAL_LINE_TO_ITEM, 0);
        ccf->show();
        ccf->setLine(false);
        currTopLanItem = 1;
    }
    if (currConnLanSsidUuidState.size() != 0) {
        int i = 0;
        do {
            actLanSsidName.append(currConnLanSsidUuidState.at(i)); //有线网络名称
            actLanUuidName.append(currConnLanSsidUuidState.at(i+1)); //有线网络唯一ID
            actLanStateName.append(currConnLanSsidUuidState.at(i+2)); //有线网络连接状态
            i += 3;
        } while(i<currConnLanSsidUuidState.size());
        currTopLanItem = actLanSsidName.size();
    }

    // 填充可用网络列表
    QString headLine = slist.at(0);
    int indexUuid, indexName;
    headLine = headLine.trimmed();

    bool isChineseExist = headLine.contains(QRegExp("[\\x4e00-\\x9fa5]+"));
    if (isChineseExist) {
        indexUuid = headLine.indexOf("UUID") + 2;
        indexName = headLine.indexOf("名称") + 2;
    } else {
        indexUuid = headLine.indexOf("UUID");
        indexName = headLine.indexOf("NAME");
    }

    for(int i = 1, j = 0; i < slist.size(); i ++) {
        QString line = slist.at(i);
        QString ltype = line.mid(0, indexUuid).trimmed();
        QString nuuid = line.mid(indexUuid, indexName - indexUuid).trimmed();
        QString nname = line.mid(indexName).trimmed();
        if (nname=="") {
            nname = " "; //防止有线网络的名称为空
        }
        bool isActiveNet = false; //isActiveNet用来表明nname是否是活动的连接

        //仅仅对有线网络进行添加列表处理
        if (ltype != "802-11-wireless" && ltype != "wifi" && ltype != "bridge" && ltype != "" && ltype != "--") {
            objKyDBus->getLanIpDNS(nuuid, true); //使用UUID获取有线网的ip和dns信息
            QString macLan = getMacByUuid(nuuid); //有线网对应的mac地址

            QString macInterface = "--";
            QString mIfName = "--";
            if (objKyDBus->dbusIfName != "--") {
                mIfName = objKyDBus->dbusIfName;
                macInterface = objKyDBus->getLanMAC(objKyDBus->dbusIfName); //有限网卡对应的mac地址

                if (macLan!="" && macLan!="--" && macLan != macInterface) {
                    //continue; //有线网的permenant mac地址与网卡的地址不同，则不在列表中显示
                    macInterface = macLan;
                }
            } else {
                mIfName = objKyDBus->multiWiredIfName.at(0); //使用默认的网络接口
                macInterface = objKyDBus->dbusMacDefault; //使用默认的MAC地址
            }

            //**********************创建已经连接的有线网item********************//
            if (currConnLanSsidUuidState.size() != 0) {//证明有已经连接的有线网络
                for (int kk=0; kk<actLanSsidName.size(); kk++) {
                    //actLanSsidName.at(kk).contains(nname)是为了防止名称中部分空格被trimmed()删除导致名称无法对应，witch caused 显示错误
                    if ((nname == actLanSsidName.at(kk) || actLanSsidName.at(kk).contains(nname)) && nuuid == actLanUuidName.at(kk) && actLanStateName.at(kk) == "connected") {
                        topLanListWidget->resize(topLanListWidget->width(), topLanListWidget->height() + H_NORMAL_ITEM*kk);
                        isActiveNet = true; //名为nname的网络是已经连接的有线网络
                        ifLanConnected = true;

                        objKyDBus->getConnectNetIp(nuuid);
                        confForm->actLanIpv6Addr = objKyDBus->dbusActiveLanIpv6;
                        getLanBandWidth();
                        //QString strLanName = TranslateLanName(nname); //进行中英文系统环境下有线网络名称的汉化

                        OneLancForm *ccfAct = new OneLancForm(topLanListWidget, this, confForm, ksnm);
                        connect(ccfAct, SIGNAL(selectedOneLanForm(QString, QString)), this, SLOT(oneTopLanFormSelected(QString, QString)));
                        connect(ccfAct, SIGNAL(disconnActiveLan()), this, SLOT(activeLanDisconn()));
                        ccfAct->setName(nname, ltype, nuuid, mIfName);//第二个参数本来是strLanName，但目前不需要翻译
                        ccfAct->setIcon(true);
                        ccfAct->setLanInfo(objKyDBus->dbusActiveLanIpv4, objKyDBus->dbusActiveLanIpv6, mwBandWidth, macInterface);
                        ccfAct->isConnected = true;
                        ccfAct->setTopItem(false);
                        ccfAct->setAct(true);
                        ccfAct->move(L_VERTICAL_LINE_TO_ITEM, kk*H_NORMAL_ITEM);
                        ccfAct->show();

                        if (actLanSsidName.size() == 1) {
                            lbLoadDown->show();
                            lbLoadUp->show();
                            lbLoadDownImg->show();
                            lbLoadUpImg->show();
                            currConnIfname = mIfName;
                            ccfAct->setConnedString(1, tr("NetOn,"), "");//"已连接"
                        } else {
                            lbLoadDown->hide();
                            lbLoadUp->hide();
                            lbLoadDownImg->hide();
                            lbLoadUpImg->hide();
                            ccfAct->setConnedString(1, tr("NetOn,IfName:"), mIfName);//"已连接"
                        }

                        if (kk != actLanSsidName.size() - 1) {
                            ccfAct->setLine(true);
                        } else {
                            ccfAct->setLine(false); //最后一个item不显示下划线
                        }
\
                        if (!objKyDBus->dbusLanIpv4.isEmpty()) {
                            if (!objKyDBus->dbusActiveLanIpv4.isEmpty() && objKyDBus->dbusActiveLanIpv4 != objKyDBus->dbusLanIpv4) {
//                                qDebug() << Q_FUNC_INFO << __LINE__ << objKyDBus->dbusActiveLanIpv4 << objKyDBus->dbusLanIpv4;
                                //在第三方nm-connection-editor进行新的IP配置后，重新连接网络
                                objKyDBus->reConnectWiredNet(nuuid);
                            } else if ((oldActLanName == actLanSsidName.at(kk)) && (oldDbusActLanDNS != objKyDBus->dbusActLanDNS)) {
                                //在第三方nm-connection-editor进行新的DNS配置后，重新连接网络
                                objKyDBus->reConnectWiredNet(nuuid);
                            } else if (!objKyDBus->dbusActiveLanIpv6.isEmpty() && objKyDBus->dbusActiveLanIpv6 != objKyDBus->dbusLanIpv6 && objKyDBus->dbusLanIpv6Method == "manual") {
//                                qDebug() << Q_FUNC_INFO << __LINE__ << objKyDBus->dbusActiveLanIpv6 << objKyDBus->dbusLanIpv6 <<  objKyDBus->dbusLanIpv6Method;
                                //在第三方nm-connection-editor或kylin-nm配置页进行新的IPV6配置后，重新连接网络
                                objKyDBus->reConnectWiredNet(nuuid);
                            }
                        }

                        currSelNetName = "";
                        objKyDBus->dbusActiveLanIpv4 = "";
                        objKyDBus->dbusActiveLanIpv6 = "";
                        oldActLanName = actLanSsidName.at(kk);
                        oldDbusActLanDNS = objKyDBus->dbusActLanDNS;
                        lbTopLanList->move(lbTopLanList->x(), lbTopLanList->y() + kk*60);
                        btnCreateNet->move(btnCreateNet->x(), btnCreateNet->y() + kk*60);
                        scrollAreal->move(scrollAreal->x(), scrollAreal->y() + kk*60);
                        syslog(LOG_DEBUG, "already insert an active lannet in the top of lan list");
                    }
                }
            }

            //**********************创建未连接的有线网item********************//
            if (!isActiveNet) {
                lanListWidget->resize(W_LIST_WIDGET, lanListWidget->height() + H_NORMAL_ITEM);
                //QString strLanName = TranslateLanName(nname);

                OneLancForm *ocf = new OneLancForm(lanListWidget, this, confForm, ksnm);
                connect(ocf, SIGNAL(selectedOneLanForm(QString, QString)), this, SLOT(oneLanFormSelected(QString, QString)));
                ocf->setName(nname, ltype, nuuid, mIfName);
                ocf->setIcon(true);
                ocf->setLine(true);
                ocf->setLanInfo(objKyDBus->dbusLanIpv4, objKyDBus->dbusLanIpv6, tr("Disconnected"), macInterface);
                ocf->setConnedString(0, tr("Disconnected"), "");//"未连接"
                ocf->move(L_VERTICAL_LINE_TO_ITEM, j * H_NORMAL_ITEM);
                ocf->setSelected(false, false);
                ocf->show();

                for (int kk=0; kk<actLanSsidName.size(); kk++) {
                    if (nname == actLanSsidName.at(kk) && nuuid == actLanUuidName.at(kk) &&  actLanStateName.at(kk) == "connecting") {
                            ocf->startWaiting(true);
                    }
                }
                j ++;
            }
        }
    }

    QList<OneLancForm *> itemList = lanListWidget->findChildren<OneLancForm *>();
    int n = itemList.size();
    if (n >= 1) {
        OneLancForm *lastItem = itemList.at(n-1);
        lastItem->setLine(false); //最后一个item不显示下划线
        lbNoItemTip->hide();
    } else {
        lbNoItemTip->show();
        lbNoItemTip->setText(tr("No Other Wired Network Scheme"));
    }

    this->lanListWidget->show();
    this->topLanListWidget->show();
    this->wifiListWidget->hide();
    this->topWifiListWidget->hide();

    this->stopLoading();
    oldLanSlist = slist;
    is_stop_check_net_state = 0;
}

// 获取wifi列表回调
void MainWindow::onRequestRevalueUpdateWifi()
{
    is_stop_check_net_state = 1;
    current_wifi_list_state = LOAD_WIFI_LIST;
}

// 获取wifi列表回调
void MainWindow::getWifiListDone(QStringList slist)
{
    //要求使用上一次获取到的列表
    if (this->ksnm->isUseOldWifiSlist) {
        slist = oldWifiSlist;
        this->ksnm->isUseOldWifiSlist = false;
    }

    if (slist.isEmpty() || slist.size() == 1) {
        if (oldWifiSlist.isEmpty() || oldWifiSlist.size() == 1) {
            this->stopLoading();
            return;
        } else {
            slist = oldWifiSlist;
        }
    }


    slist = priorityList(slist);
    if (isHuaWeiPC) {
        if (slist.size() >= 2) {
            wifiListOptimize(slist);
//            getFinalWifiList(slist);
        }
    }

    if (current_wifi_list_state == RECONNECT_WIFI) {
        QList<structWifiProperty> targetWifiStructList = connectableWifiPriorityList(slist);
        if (!targetWifiStructList.isEmpty()) {
            if (!isReconnectingWifi) {
                isReconnectingWifi = true; //保证对于连续发出的重连信号，只处理第一个
                QtConcurrent::run([=]() {
                    QString wifiSsid = objKyDBus->getWifiSsid(targetWifiStructList.at(0).objectPath);
                    QString modityCmd = "nmcli connection modify \""+ wifiSsid + "\" " + "802-11-wireless.bssid " + targetWifiStructList.at(0).bssid;
                    system(modityCmd.toUtf8().data());
                    QString reconnectWifiCmd = "nmcli connection up \"" + wifiSsid + "\"";
                    system(reconnectWifiCmd.toUtf8().data());
                });
            }
        }
        current_wifi_list_state = LOAD_WIFI_LIST;
    }

    if (this->is_btnLanList_clicked == 1) {
        return;
    }
    if (current_wifi_list_state == LOAD_WIFI_LIST) {
        //qDebug() << "loadwifi的列表";
        loadWifiListDone(slist);
        is_init_wifi_list = 0;
    }

    if (current_wifi_list_state == UPDATE_WIFI_LIST) {
        //qDebug() << "updatewifi的列表";
        updateWifiListDone(slist);
        current_wifi_list_state = LOAD_WIFI_LIST;
    }

    oldWifiSlist = slist;
}

// 获取已保存的连接列表回调
void MainWindow::getConnListDone(QStringList slist)
{
    if (isInitConnList) {
        for (int i = 1; i < slist.length() - 1; i++) {
            oldConnSlist << slist.at(i).trimmed();
        }
        isInitConnList = false;
        return;
    } else {
        QStringList newConnSlist;
        for (int i = 1; i < slist.length() - 1; i++) {
            newConnSlist << slist.at(i).trimmed();
        }
        for (auto s : newConnSlist) {
            if (!oldConnSlist.contains(s)) {
                lastAddedConn = s;
                break;
            }
        }
        if (isAddedWifi) {
            isAddedWifi = false;
            //如果是新添加的wifi，尝试激活这个wifi
            if (! is_stop_check_net_state) {
                this->is_stop_check_net_state = 1;
                BackThread *bt = new BackThread();
                bt->execConnWifi(lastAddedConn, objKyDBus->dbusWiFiCardName);
                connect(bt, SIGNAL(connDone(int)), this, SLOT(connWifiDone(int)));
            }
        }
        oldConnSlist.clear();
        oldConnSlist = newConnSlist;
        return;
    }
}
QStringList MainWindow::priorityList(QStringList slist){
    QStringList ret;
    ret.append(slist[0]);
    QString headLine = slist.at(0);
    int indexSignal,indexSecu, indexFreq, indexBSsid, indexName,indexPath,indexCate;
    headLine = headLine.trimmed();
    bool isChineseExist = headLine.contains(QRegExp("[\\x4e00-\\x9fa5]+"));
    if (isChineseExist) {
        indexSignal = headLine.indexOf("SIGNAL");
        indexSecu = headLine.indexOf("安全性");
        indexFreq = headLine.indexOf("频率") + 4;
        indexBSsid = headLine.indexOf("BSSID") + 6;
        indexName = indexBSsid + 19;
        indexPath = headLine.indexOf("DBUS-PATH");
        indexCate = headLine.indexOf("CATEGORY");
    } else {
        indexSignal = headLine.indexOf("SIGNAL");
        indexSecu = headLine.indexOf("SECURITY");
        indexFreq = headLine.indexOf("FREQ");
        indexBSsid = headLine.indexOf("BSSID");
        indexName = indexBSsid + 19;
        indexPath = headLine.indexOf("DBUS-PATH");
        indexCate = headLine.indexOf("CATEGORY");
    }
    QStringList p1,p2,p3,p4,p5,p6,p7;//按照信号与频段划分为6个列表
    for(int i=1;i<slist.size();i++){
        QString line = slist.at(i);
        int conSignal = line.mid(indexSignal,3).trimmed().toInt();
        int conFreq = line.mid(indexFreq,4).trimmed().toInt();
        if(conSignal > 75 && conFreq >= 5000){
            p1.append(line);
            continue;
        }else if(conSignal > 55 && conFreq >= 5000){
            p2.append(line);
            continue;
        }else if(conSignal > 75){
            p3.append(line);
            continue;
        }else if(conSignal > 55){
            p4.append(line);
            continue;
        }else if(conSignal > 35){
            p5.append(line);
            continue;
        }else if(conSignal > 15){
            p6.append(line);
            continue;
        }else{
            p7.append(line);
        }
    }
    QVector<QStringList> listVec;
    listVec<<p1<<p2<<p3<<p4<<p5<<p6<<p7;
    for(auto list:listVec){
        list = sortApByCategory(list,indexCate);
        ret += list;
    }
    return ret;
}
QVector<QStringList> MainWindow::repetitionFilter(QVector<QStringList>){
    QVector<QStringList> ret;
    return ret;
}
QStringList MainWindow::sortApByCategory(QStringList list,int cateIndex){
    QStringList ret;
    for(auto line:list){
        int conCate = line.mid(cateIndex).trimmed().toInt();
        if(conCate == 2){
            ret.append(line);
        }
    }
    for(auto line:list){
        int conCate = line.mid(cateIndex).trimmed().toInt();
        if(conCate == 1){
            ret.append(line);
        }
    }
    for(auto line:list){
        int conCate = line.mid(cateIndex).trimmed().toInt();
        if(conCate == 0){
            ret.append(line);
        }
    }
    return ret;
}
//进行wifi列表优化选择，分为2.4G和5G进行选择，先每种频率形成一个列表
//同一个列表中同名wifi只有一个，再按信号强度由大到小合并列表
void MainWindow::wifiListOptimize(QStringList& slist)
{
    if(slist.size() < 2) return ;
    //这个函数可能会把已经连接的那个wifi给筛选出去
    QString headLine = slist.at(0);
    int indexSignal,indexSecu, indexFreq, indexBSsid, indexName,indexPath;
    headLine = headLine.trimmed();

    bool isChineseExist = headLine.contains(QRegExp("[\\x4e00-\\x9fa5]+"));
    if (isChineseExist) {
        indexSignal = headLine.indexOf("SIGNAL");
        indexSecu = headLine.indexOf("安全性");
        indexFreq = headLine.indexOf("频率") + 4;
        indexBSsid = headLine.indexOf("BSSID") + 6;
        indexName = indexBSsid + 19;
        indexPath = headLine.indexOf("DBUS-PATH");
    } else {
        indexSignal = headLine.indexOf("SIGNAL");
        indexSecu = headLine.indexOf("SECURITY");
        indexFreq = headLine.indexOf("FREQ");
        indexBSsid = headLine.indexOf("BSSID");
        indexName = indexBSsid + 19;
        indexPath = headLine.indexOf("DBUS-PATH");
    }
    QStringList targetList; //slist优化，同名同频AP中只留信号最强
    targetList<<slist.at(0);    //把第一行加进去
    for(int it = 1;it < slist.size();it++){
        QString currentWifiInfo = slist.at(it);
        bool ifContinue = false;
        QString conName = currentWifiInfo.mid(indexName, indexPath - indexName).trimmed();
        int conSignal = currentWifiInfo.mid(indexSignal,3).trimmed().toInt();
        int conFreq = currentWifiInfo.mid(indexFreq,4).trimmed().toInt();
        for(int i=0;i<slist.size();i++){
            QString str = slist.at(i);
            QString name = str.mid(indexName, indexPath - indexName).trimmed();
            int signal = str.mid(indexSignal,3).trimmed().toInt();
            int freq = str.mid(indexFreq,4).trimmed().toInt();
            if(conName == name){
                if(conFreq < 5000 && freq < 5000){
                    if(conSignal < signal){
                        ifContinue = true;
                        break;
                    }
                }
                if(conFreq >= 5000 && freq >= 5000){
                    if(conSignal < signal){
                        ifContinue = true;
                        break;
                    }
                }
            }
        }
        if(ifContinue)  continue;
        targetList<<currentWifiInfo;
    }
    slist = targetList;
}

//最终优选出来的wifi列表
void MainWindow::getFinalWifiList(QStringList &slist)
{
    if(slist.size() < 2) return ;
    QString headLine = slist.at(0);
    int indexSignal,indexSecu, indexFreq, indexBSsid, indexName,indexPath;
    headLine = headLine.trimmed();
    bool isChineseExist = headLine.contains(QRegExp("[\\x4e00-\\x9fa5]+"));
    if (isChineseExist) {
        indexSignal = headLine.indexOf("SIGNAL");
        indexSecu = headLine.indexOf("安全性");
        indexFreq = headLine.indexOf("频率") + 4;
        indexBSsid = headLine.indexOf("BSSID") + 6;
        indexName = indexBSsid + 19;
        indexPath = headLine.indexOf("DBUS-PATH");
    } else {
        indexSignal = headLine.indexOf("SIGNAL");
        indexSecu = headLine.indexOf("SECURITY");
        indexFreq = headLine.indexOf("FREQ");
        indexBSsid = headLine.indexOf("BSSID");
        indexName = indexBSsid + 19;
        indexPath = headLine.indexOf("DBUS-PATH");
    }

    QStringList deleteWifiStr;
    for(int ii = 1;ii < slist.size();ii++){
        if ((ii+1) == slist.size()) {
            break;
        }
        QString wifiInfo = slist.at(ii);
        QString conName = wifiInfo.mid(indexName, indexPath - indexName).trimmed();
        int conSignal = wifiInfo.mid(indexSignal,3).trimmed().toInt();
        int conFreq = wifiInfo.mid(indexFreq,4).trimmed().toInt();
        for(int jj=ii+1;jj<slist.size();jj++){
            QString wifiStr = slist.at(jj);
            QString name = wifiStr.mid(indexName, indexPath - indexName).trimmed();
            int signal = wifiStr.mid(indexSignal,3).trimmed().toInt();
            int freq = wifiStr.mid(indexFreq,4).trimmed().toInt();
            if(conName == name){
                if (conFreq >= 5000) {
                    //排在前面的一个是5Gwifi，并且信号也强一些
                    deleteWifiStr.append(wifiStr);
                } else {
                    //排在前面的一个是2.4G
                    if (freq >= 5000) {
                        //排在后面的一个是5G
                        if (signal >= 55) {
                            //排在后面的一个是5Gwifi信号强度 >= 3格，选5G
                            deleteWifiStr.append(wifiInfo);
                        } else {
                            deleteWifiStr.append(wifiStr);
                        }
                    } else {
                        //排在后面的一个是2.4G
                        deleteWifiStr.append(wifiStr);
                    }
                }
                break;
            }
        }
    }
    foreach (QString deleteStr, deleteWifiStr) {
        slist.removeOne(deleteStr);
    }
}

//从有配置文件的wifi选出最优wifi进行连接,同时考虑用户手动设置的优先级,这个函数在回连中用到
QList<structWifiProperty> MainWindow::connectableWifiPriorityList(const QStringList slist){
    QList<structWifiProperty> selectedWifiListStruct;
    if(slist.size()<2) return selectedWifiListStruct;
    OneConnForm *ocf = new OneConnForm();
    QString headLine = slist.at(0);
    int indexSignal,indexSecu, indexFreq, indexBSsid, indexName, indexPath,indexCate;
    headLine = headLine.trimmed();

    bool isChineseExist = headLine.contains(QRegExp("[\\x4e00-\\x9fa5]+"));
    if (isChineseExist) {
        indexSignal = headLine.indexOf("SIGNAL");
        indexSecu = headLine.indexOf("安全性");
        indexFreq = headLine.indexOf("频率") + 4;
        indexBSsid = headLine.indexOf("BSSID") + 6;
        indexName = indexBSsid + 19;
        indexPath = headLine.indexOf("DBUS-PATH");
        indexCate = headLine.indexOf("CATEGORY");
    } else {
        indexSignal = headLine.indexOf("SIGNAL");
        indexSecu = headLine.indexOf("SECURITY");
        indexFreq = headLine.indexOf("FREQ");
        indexBSsid = headLine.indexOf("BSSID");
        indexName = indexBSsid + 19;
        indexPath = headLine.indexOf("DBUS-PATH");
        indexCate = headLine.indexOf("CATEGORY");
    }

    QStringList tmp = slist;
    for (int iter=1; iter<tmp.size(); iter++) {
        QString line = tmp.at(iter);
        QString wifiname = line.mid(indexName,indexPath - indexName).trimmed();
        QString wifibssid = line.mid(indexBSsid, indexName-indexBSsid).trimmed();
        QString wifiObjectPath = line.mid(indexPath,indexCate-indexPath).trimmed();
        QString wifiAutoConnection;
        QString wifiPriority;

        if (ocf->isWifiConfExist(wifiname) && canReconnectWifiList.contains(wifiname)) {
            QString tmpPath = "/tmp/kylin-nm-lanprop-" + QDir::home().dirName();
            QString getInfoCmd = "nmcli connection show '" + wifiname + "' > " + tmpPath;
            Utils::m_system(getInfoCmd.toUtf8().data());
            QFile file(tmpPath);
            if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                syslog(LOG_ERR, "Can't open the file /tmp/kylin-nm-lanprop in function connectableWifiPriorityList!");
                qDebug()<<"Can't open the file /tmp/kylin-nm-lanprop in function connectableWifiPriorityList!";
            }
            QString txt = file.readAll();
            QStringList txtLine = txt.split("\n");
            file.close();
            foreach (QString line, txtLine) {
                if (line.startsWith("connection.autoconnect:")) {
                    wifiAutoConnection = line.mid(25).trimmed(); //connection.autoconnect:有23个字符
                }
                if (line.startsWith("connection.autoconnect-priority:")) {
                    wifiPriority = line.mid(34).trimmed(); //connection.autoconnect-priority:有32个字符
                }
            }

            //可以自动回连，则加入列表
            if (wifiAutoConnection == "是" || wifiAutoConnection == "yes") {
                structWifiProperty myWifiProStruct;
                myWifiProStruct.objectPath = wifiObjectPath;
                myWifiProStruct.bssid = wifibssid;
                myWifiProStruct.priority = wifiPriority.toInt();
                selectedWifiListStruct.append(myWifiProStruct);
            }
        }
    }

    //再按照wifiPriority进行排序，先简单的选出最高优先级的wifi进行连接
    if (selectedWifiListStruct.size() > 1) {
        devListSort(&selectedWifiListStruct);
        foreach (structWifiProperty wifiPriorityAfterSort, selectedWifiListStruct) {
            qDebug() << wifiPriorityAfterSort.bssid << " 的自动连接优先级是 ： " << wifiPriorityAfterSort.priority;
        }
    }
    ocf->deleteLater();
    return selectedWifiListStruct;
}

//排序
void MainWindow::devListSort(QList<structWifiProperty> *list)
{
    qSort(list->begin(), list->end(), subDevListSort);
}
bool MainWindow::subDevListSort(const structWifiProperty &info1, const structWifiProperty &info2)
{
   return info1.priority > info2.priority;
}

// 加载wifi列表
void MainWindow::loadWifiListDone(QStringList slist)
{
    delete topWifiListWidget; //清空top列表
    createTopWifiUI(); //创建topWifiListWidget
    // 清空wifi列表
    wifiListWidget = new QWidget(scrollAreaw);
    wifiListWidget->resize(W_LIST_WIDGET, H_WIFI_ITEM_BIG_EXTEND);
    scrollAreaw->setWidget(wifiListWidget);
    scrollAreaw->move(W_LEFT_AREA, Y_SCROLL_AREA);

    // 获取当前有线网的连接状态，正在连接wifiActState==1，已经连接wifiActState==2, 未连接wifiActState==3
    int wifiActState = objKyDBus->checkWifiConnectivity(); //检查wifi的连接状态
//    if (isWifiBeConnUp && wifiActState == 1) {
//        wifiActState = 2;
//    }

    QList<QString> currConnWifiBSsidUuid;
    currConnWifiBSsidUuid = objKyDBus->getAtiveWifiBSsidUuid(slist);
//    bool isLoop = true;
//    do {
//        currConnWifiBSsidUuid = objKyDBus->getAtiveWifiBSsidUuid(slist);
//        if (currConnWifiBSsidUuid.size() == 1 && currConnWifiBSsidUuid.at(0).length() != 17) {
//            sleep(1); //等于1说明只获取到uuid，1秒后再获取一次
//        } else {
//            isLoop = false;
//        }
//    } while (isLoop);

    // 获取当前连接的wifi name
    QString actWifiName = "--";
    QString actWifiId = "--";
    actWifiSsid = "--";
    actWifiUuid = "--";
    if (currConnWifiBSsidUuid.size() > 1) {
        actWifiUuid = currConnWifiBSsidUuid.at(0);
        for (int i=1; i<currConnWifiBSsidUuid.size(); i++) {
            actWifiBssidList.append(currConnWifiBSsidUuid.at(i));
            //qDebug() << "debug: 获取到的bssid是：" << currConnWifiBSsidUuid.at(i);
        }
        //qDebug() << "debug: 获取到的uuid是：" << actWifiUuid;
    } else {
        actWifiBssidList.append("--");
    }

    activecon *act = kylin_network_get_activecon_info();
    int index = 0;
    while (act[index].con_name != NULL) {
        if (QString(act[index].type) == "wifi" || QString(act[index].type) == "802-11-wireless") {
            actWifiName = QString(act[index].con_name);
            break;
        }
        index ++;
    }

    // 根据当前连接的wifi 设置OneConnForm
    OneConnForm *ccf = new OneConnForm(topWifiListWidget, this, confForm, ksnm);
    if (actWifiName == "--" || wifiActState == 1 || actWifiBssidList.at(0) == "--") {
        ccf->setName(tr("Not connected"), "--", "--", "--");//"当前未连接任何 Wifi"
        ccf->setSignal("0", "--");
        activeWifiSignalLv = 0;
        ccf->setConnedString(1, tr("Disconnected"), "");//"未连接"
        ccf->isConnected = false;
        ccf->lbFreq->hide();
        ifWLanConnected = false;
        lbLoadDown->hide();
        lbLoadUp->hide();
        lbLoadDownImg->hide();
        lbLoadUpImg->hide();
        ccf->setTopItem(false);
    } else {
        QProcess * process = new QProcess;
        QString name = actWifiName;
        process->start(QString("nmcli -f 802-11-wireless.ssid connection show \"%1\"").arg(name));
        connect(process, static_cast<void(QProcess::*)(int,QProcess::ExitStatus)>(&QProcess::finished), this, [ = ]() {
            process->deleteLater();
        });
        connect(process, &QProcess::readyReadStandardOutput, this, [ = ]() {
            QString str = process->readAllStandardOutput();
            actWifiSsid = str.mid(str.lastIndexOf(" ") + 1, str.length() - str.lastIndexOf(" ") - 2); //获取到ssid时，以ssid为准
        });
        connect(process, &QProcess::readyReadStandardError, this, [ = ]() {
            actWifiSsid = actWifiName; //没有获取到ssid时，以wifi名为准
        });
        process->waitForFinished();
    }
    ccf->setAct(true);
    ccf->move(L_VERTICAL_LINE_TO_ITEM, 0);
    ccf->show();

    // 填充可用网络列表
    QString headLine = slist.at(0);
    int indexSignal,indexSecu, indexFreq, indexBSsid, indexName, indexPath, indexCate;
    headLine = headLine.trimmed();

    bool isChineseExist = headLine.contains(QRegExp("[\\x4e00-\\x9fa5]+"));
    if (isChineseExist) {
        indexSignal = headLine.indexOf("SIGNAL");
        indexSecu = headLine.indexOf("安全性");
        indexFreq = headLine.indexOf("频率") + 4;
        indexBSsid = headLine.indexOf("BSSID") + 6;
        indexName = indexBSsid + 19;
        indexPath = headLine.indexOf("DBUS-PATH");
        indexCate = headLine.indexOf("CATEGORY");
    } else {
        indexSignal = headLine.indexOf("SIGNAL");
        indexSecu = headLine.indexOf("SECURITY");
        indexFreq = headLine.indexOf("FREQ");
        indexBSsid = headLine.indexOf("BSSID");
        indexName = indexBSsid + 19;
        indexPath = headLine.indexOf("DBUS-PATH");
        indexCate = headLine.indexOf("CATEGORY");
    }
    QStringList wnames;
    int count = 0;
    QString actWifiBssid = " ";
    for (int i = 1; i < slist.size(); i ++) {
        QString line = slist.at(i);
        QString wbssid = line.mid(indexBSsid, 17).trimmed();
        QString wname = line.mid(indexName, indexPath - indexName).trimmed();
        int chineseNum = 0;
        for(QChar text:wname){
            if(text.unicode() > 0x4E00 && text.unicode() <= 0x9FFF){//统计字符串中中文字符的个数
                chineseNum++;
            }
        }
        wname = line.mid(indexName, indexPath - indexName - chineseNum).trimmed();
        if (actWifiBssidList.contains(wbssid)) {
            actWifiName = wname;
        }
        if ("*" == line.mid(0,indexSignal).trimmed()) {
            //在华为的电脑中，因为前面的优选工作，即使有已经连接的wifi，也可能会被筛选出去
            actWifiBssid = wbssid;
        }
    }

    if (actWifiBssidList.size()==1 && actWifiBssidList.at(0)=="--") {
        actWifiId = actWifiName;
        actWifiName = "--";
    }
    for (int i = 1, j = 0; i < slist.size(); i ++) {
        QString line = slist.at(i);
        QString wsignal = line.mid(indexSignal, 3).trimmed();
        QString wsecu = line.mid(indexSecu, indexFreq - indexSecu).trimmed();
        QString wbssid = line.mid(indexBSsid, 17).trimmed();
        QString wname = line.mid(indexName, indexPath - indexName).trimmed();
        QString wfreq = line.mid(indexFreq, 4).trimmed();
        QString wcate = line.mid(indexCate).trimmed();
        int chineseNum = 0;
        for(QChar text:wname){
            if(text.unicode() > 0x4E00 && text.unicode() <= 0x9FFF){//统计字符串中中文字符的个数
                chineseNum++;
            }
        }
        wname = line.mid(indexName, indexPath - indexName - chineseNum).trimmed();

        if (!isHuaWeiPC) {
            //如果不是华为的电脑，选择wifi在这里执行
            if (actWifiName != "--" && actWifiName == wname) {
                if (!actWifiBssidList.contains(wbssid)) {
                    continue; //若当前热点ssid名称和已经连接的wifi的ssid名称相同，但bssid不同，则跳过
                }
            }
            if ((wnames.contains(wname) && wbssid != actWifiBssid)) {
                continue; //过滤相同名称的wifi
            }
        } else {
            if ((actWifiName == wname) && actWifiBssidList.size()>=1) {
                //防止列表中没有已经连接的那个wifi
                wbssid = actWifiBssidList.at(0);
                actWifiBssid = actWifiBssidList.at(0);
            }
            if ((wnames.contains(wname) && wbssid != actWifiBssid)) {
                continue; //过滤相同名称的wifi
            }
        }

        int max_freq = wfreq.toInt();
        int min_freq = wfreq.toInt();
        for (int k = i; k < slist.size(); k ++) {
            QString m_name = slist.at(k).mid(indexName, indexPath - indexName).trimmed();
            int chineseNum = 0;
            for(QChar text:wname){
                if(text.unicode() > 0x4E00 && text.unicode() <= 0x9FFF){//统计字符串中中文字符的个数
                    chineseNum++;
                }
            }
            wname = line.mid(indexName, indexPath - indexName - chineseNum).trimmed();

            if (wname == m_name) {
                if (slist.at(k).mid(indexFreq, 4).trimmed().toInt() > max_freq) {
                    max_freq = slist.at(k).mid(indexFreq, 4).trimmed().toInt();
                } else if (slist.at(k).mid(indexFreq, 4).trimmed().toInt() < min_freq) {
                    min_freq = slist.at(k).mid(indexFreq, 4).trimmed().toInt();
                }
            }
        }

        int freqState = 0;
        if (max_freq < 3000) {
            //只有2.4GHZ
            freqState = 1;
        } else if (min_freq >= 5000) {
            //只有5GHZ
            freqState = 2;
        }
        if (wname != "" && wname != "--") {
            //qDebug() << "wifi的 bssid: " << wbssid << "当前连接的wifi的bssid: " << actWifiBssidList;
            if(actWifiBssid == wbssid && wifiActState == 2){
                //对于已经连接的wifi
                connect(this, &MainWindow::actWifiSignalLvChanaged, ccf, [ = ](const int &signalLv) {
                    ccf->setSignal(QString::number(signalLv), wsecu);
                });
                connect(ccf, SIGNAL(selectedOneWifiForm(QString,int)), this, SLOT(oneTopWifiFormSelected(QString,int)));
                connect(ccf, SIGNAL(disconnActiveWifi()), this, SLOT(activeWifiDisconn()));
                QString path = line.mid(indexPath).trimmed();
                QString m_name;
                if (path != "" && !path.isEmpty()) m_name= this->objKyDBus->getWifiSsid(QString("/org/freedesktop/NetworkManager/AccessPoint/%1").arg(path.mid(path.lastIndexOf("/") + 1)));
                if (m_name.isEmpty() || m_name == "") {
                    ccf->setName(wname, wbssid, actWifiUuid, objKyDBus->dbusWiFiCardName);
                    if (!canReconnectWifiList.contains(wname)) {
                        canReconnectWifiList.append(wname);
                    } else {
                        canReconnectWifiList.removeOne(wname);
                        canReconnectWifiList.append(wname);
                    }
                } else {
                    ccf->setName(m_name, wbssid, actWifiUuid, objKyDBus->dbusWiFiCardName);
                    if (!canReconnectWifiList.contains(m_name)) {
                        canReconnectWifiList.append(m_name);
                    } else {
                        canReconnectWifiList.removeOne(m_name);
                        canReconnectWifiList.append(m_name);
                    }
                }

                //ccf->setRate(wrate);
                int signal = wsignal.toInt() + 11;
                ccf->setSignal(QString::number(signal), wsecu);
                setTrayIconOfWifi(wsignal.toInt());
                activeWifiSignalLv = wsignal.toInt();
                //objKyDBus->getWifiMac(wname);
                ccf->setWifiInfo(wsecu, wsignal, wbssid, freqState);
                ccf->setConnedString(1, tr("NetOn,"), wsecu);//"已连接"
                ccf->isConnected = true;
                ifWLanConnected = true;
                lbLoadDown->show();
                lbLoadUp->show();
                lbLoadDownImg->show();
                lbLoadUpImg->show();
                ccf->setTopItem(false);
                currSelNetName = "";
                syslog(LOG_DEBUG, "already insert an active wifi in the top of wifi list");
            } else {
                //对于未连接的wifi
                wifiListWidget->resize(W_LIST_WIDGET, wifiListWidget->height() + H_NORMAL_ITEM);

                OneConnForm *ocf = new OneConnForm(wifiListWidget, this, confForm, ksnm);
                connect(ocf, SIGNAL(selectedOneWifiForm(QString,int)), this, SLOT(oneWifiFormSelected(QString,int)));
//                ocf->setName(wname, wbssid, "--");
                QString path = line.mid(indexPath).trimmed();
                QString m_name;
                if (path != "" && !path.isEmpty()) m_name= this->objKyDBus->getWifiSsid(QString("/org/freedesktop/NetworkManager/AccessPoint/%1").arg(path.mid(path.lastIndexOf("/") + 1)));
                if (m_name.isEmpty() || m_name == "") {
                    ocf->setName(wname, wbssid, actWifiUuid, objKyDBus->dbusWiFiCardName);
                } else {
                    ocf->setName(m_name, wbssid, actWifiUuid, objKyDBus->dbusWiFiCardName);
                }
                //ocf->setRate(wrate);
                ocf->setLine(true);
                ocf->setSignal(wsignal, wsecu, wcate);
                //objKyDBus->getWifiMac(wname);
                ocf->setWifiInfo(wsecu, wsignal, wbssid, freqState);
                ocf->setConnedString(0, tr("Disconnected"), wsecu);
                ocf->move(L_VERTICAL_LINE_TO_ITEM, j * H_NORMAL_ITEM);
                ocf->setSelected(false, false);
                ocf->show();

                if (actWifiBssidList.contains(wbssid) && wifiActState == 1) {
                    ocf->startWifiWaiting(true);
                } else if (actWifiId == wname && wifiActState == 1) {
                    ocf->startWifiWaiting(true);
                }

                j ++;
                count ++;
            }

            wnames.append(wname);
        }
    }

    QList<OneConnForm *> itemList = wifiListWidget->findChildren<OneConnForm *>();
    int n = itemList.size();
    if (n >= 1) {
        OneConnForm *lastItem = itemList.at(n-1);
        lastItem->setLine(false); //最后一个item不需要下划线
        lbNoItemTip->hide();
        lbTopWifiList->show();
        btnAddNet->show();
    } else {
        if (ifWLanConnected) {
            lbNoItemTip->show();
            lbNoItemTip->setText(tr("No Other Wireless Network Scheme"));
        } else {
            lbNoItemTip->hide();
            lbTopWifiList->hide();
            btnAddNet->hide();
        }
    }

    this->lanListWidget->hide();
    this->topLanListWidget->hide();
    this->wifiListWidget->show();
    this->topWifiListWidget->show();

    this->stopLoading();
    is_stop_check_net_state = 0;
    is_connect_hide_wifi = 0;

    actWifiBssidList.clear();
    wnames.clear();
}

// 更新wifi列表
void MainWindow::updateWifiListDone(QStringList slist)
{
    if (hasWifiConnected) {
        lbLoadDown->show();
        lbLoadUp->show();
        lbLoadDownImg->show();
        lbLoadUpImg->show();
    }

    if (this->ksnm->isExecutingGetLanList){ return;}

    //获取表头信息
    QString lastHeadLine = oldWifiSlist.at(0);
    int lastIndexName, lastIndexPath;
    lastHeadLine = lastHeadLine.trimmed();
    bool isChineseInIt = lastHeadLine.contains(QRegExp("[\\x4e00-\\x9fa5]+"));
    if (isChineseInIt) {
        lastIndexName = lastHeadLine.indexOf("BSSID") + 6 + 19;
    } else {
        lastIndexName = lastHeadLine.indexOf("BSSID") + 19;
    }
    lastIndexPath = lastHeadLine.indexOf("DBUS-PATH");

    QString headLine = slist.at(0);
    int indexSecu, indexFreq, indexBSsid, indexName, indexPath, indexCate;
    headLine = headLine.trimmed();
    bool isChineseExist = headLine.contains(QRegExp("[\\x4e00-\\x9fa5]+"));
    if (isChineseExist) {
        indexSecu = headLine.indexOf("安全性");
        indexFreq = headLine.indexOf("频率") + 4;
        indexBSsid = headLine.indexOf("BSSID") + 6;
        //indexName = headLine.indexOf("SSID") + 6;
        indexName = indexBSsid + 19;
        indexPath = headLine.indexOf("DBUS-PATH");
        indexCate= headLine.indexOf("CATEGORY");
    } else {
        indexSecu = headLine.indexOf("SECURITY");
        indexFreq = headLine.indexOf("FREQ");
        indexBSsid = headLine.indexOf("BSSID");
        //indexName = headLine.indexOf("SSID");
        indexName = indexBSsid + 19;
        indexPath = headLine.indexOf("DBUS-PATH");
        indexCate = headLine.indexOf("CATEGORY");
    }

    //列表中去除已经减少的wifi
    for (int i=1; i<oldWifiSlist.size(); i++){
        QString line = oldWifiSlist.at(i);
        QString lastWname = line.mid(lastIndexName, lastIndexPath - lastIndexName).trimmed();
        for (int j=1; j<slist.size(); j++){
            QString line = slist.at(j);
            QString wname = line.mid(indexName, indexPath - indexName).trimmed();

            if (lastWname == wname){break;} //在slist最后之前找到了lastWname，则停止
            if (j == slist.size()-1) {
                QList<OneConnForm *> wifiList = wifiListWidget->findChildren<OneConnForm *>();
                for (int pos = 0; pos < wifiList.size(); pos ++) {
                    OneConnForm *ocf = wifiList.at(pos);
                    if (ocf->getName() == lastWname) {
                        if (ocf->isActive == true){break;
                        } else {
                            delete ocf;
                            //删除元素下面的的所有元素上移
                            for (int after_pos = pos+1; after_pos < wifiList.size(); after_pos ++) {
                                OneConnForm *after_ocf = wifiList.at(after_pos);
                                if (lastWname == currSelNetName) {after_ocf->move(L_VERTICAL_LINE_TO_ITEM, after_ocf->y() - H_NORMAL_ITEM - H_WIFI_ITEM_BIG_EXTEND);}
                                else {after_ocf->move(L_VERTICAL_LINE_TO_ITEM, after_ocf->y() - H_NORMAL_ITEM);}
                            }
                            wifiListWidget->resize(W_LIST_WIDGET, wifiListWidget->height() - H_NORMAL_ITEM);
                            break;
                        }
                    }
                }

            } //end if (j == slist.size()-1)
        } //end (int j=1; j<slist.size(); j++)
    }

    //列表中插入新增的wifi
    QStringList wnames;
    int count = 0;
    for(int i = 1; i < slist.size(); i++){
        QString line = slist.at(i);
        QString wsignal = line.mid(0, indexSecu).trimmed();
        QString wsecu = line.mid(indexSecu, indexFreq - indexSecu).trimmed();
        QString wbssid = line.mid(indexBSsid, 17).trimmed();
        QString wname = line.mid(indexName, indexPath - indexName).trimmed();
        QString wfreq = line.mid(indexFreq, 4).trimmed();
        QString wcate =line.mid(indexCate).trimmed();

        if(wname == "" || wname == "--"){continue;}

        bool isContinue = false;
        foreach (QString addName, wnames) {
            // 重复的网络名称，跳过不处理
            if(addName == wname){isContinue = true;}
        }
        if(isContinue){continue;}

        int max_freq = wfreq.toInt();
        int min_freq = wfreq.toInt();
        for (int k = 0; k < slist.size(); k ++) {
            QString m_name = slist.at(k).mid(indexName, indexPath - indexName).trimmed();
//            QString m_name = this->objKyDBus->getWifiSsid(QString("/org/freedesktop/NetworkManager/AccessPoint/%1").arg(slist.at(k).mid(slist.at(k).lastIndexOf("/") + 1).trimmed()));
            if (wname == m_name) {
                if (slist.at(k).mid(indexFreq, 4).trimmed().toInt() > max_freq) {
                    max_freq = slist.at(k).mid(indexFreq, 4).trimmed().toInt();
                } else if (slist.at(k).mid(indexFreq, 4).trimmed().toInt() < min_freq) {
                    min_freq = slist.at(k).mid(indexFreq, 4).trimmed().toInt();
                }
            }
        }

        int freqState = 0;
        if (max_freq < 3000) {
            //只有2.4GHZ
            freqState = 1;
        } else if (min_freq >= 5000) {
            //只有5GHZ
            freqState = 2;
        }

        wnames.append(wname);

        for (int j=1; j < oldWifiSlist.size(); j++) {
            QString line = oldWifiSlist.at(j);
            QString lastWname = line.mid(lastIndexName, lastIndexPath - lastIndexName).trimmed();
            if (lastWname == wname){break;} //上一次的wifi列表已经有名为wname的wifi，则停止
            if (j == oldWifiSlist.size()-1) { //到lastSlist最后一个都没找到，执行下面流程
                QList<OneConnForm *> wifiList = wifiListWidget->findChildren<OneConnForm *>();
                int n = wifiList.size();
                int posY = 0;
                if (n >= 1) {
                    OneConnForm *lastOcf = wifiList.at(n-1);
                    lastOcf->setLine(true);
                    if (lastOcf->wifiName == currSelNetName) {
                        posY = lastOcf->y()+H_NORMAL_ITEM + H_WIFI_ITEM_BIG_EXTEND;
                    } else {
                        posY = lastOcf->y()+H_NORMAL_ITEM;
                    }
                }

                wifiListWidget->resize(W_LIST_WIDGET, wifiListWidget->height() + H_NORMAL_ITEM);
                OneConnForm *addItem = new OneConnForm(wifiListWidget, this, confForm, ksnm);
                connect(addItem, SIGNAL(selectedOneWifiForm(QString,int)), this, SLOT(oneWifiFormSelected(QString,int)));
                QString path = line.mid(indexPath).trimmed();
                QString m_name;
                if (path != "" && !path.isEmpty()) m_name= this->objKyDBus->getWifiSsid(QString("/org/freedesktop/NetworkManager/AccessPoint/%1").arg(path.mid(path.lastIndexOf("/") + 1)));
                if (m_name.isEmpty() || m_name == "") {
                    addItem->setName(wname, wbssid, actWifiUuid, objKyDBus->dbusWiFiCardName);
                } else {
                    addItem->setName(m_name, wbssid, actWifiUuid, objKyDBus->dbusWiFiCardName);
                }
                //addItem->setRate(wrate);
                addItem->setLine(false);
                addItem->setSignal(wsignal, wsecu, wcate);
                //objKyDBus->getWifiMac(wname);
                addItem->setWifiInfo(wsecu, wsignal, wbssid, freqState);
                addItem->setConnedString(0, tr("Disconnected"), wsecu);//"未连接"
                addItem->move(L_VERTICAL_LINE_TO_ITEM, posY);
                addItem->setSelected(false, false);
                addItem->show();

                count += 1;
            }
        }
    }

    this->lanListWidget->hide();
    this->topLanListWidget->hide();
    this->wifiListWidget->show();
    this->topWifiListWidget->show();
    this->stopLoading();
}

//用于中英文系统有线网络名称国际话
QString MainWindow::TranslateLanName(QString lanName)
{
    QString returnLanName = lanName;

    if (lanName == "有线连接") {
        returnLanName = tr("Wired connection");
    } else if (lanName.indexOf("有线连接") != -1) {
        QStringList strList = lanName.split(" ");
        if (strList.size() >= 2 && strList.at(1) != "") {
            returnLanName = tr("Wired connection") + " " + strList.at(1);
        }
    }

    if (lanName == "Wired connection") {
        returnLanName = tr("Wired connection");
    } else if (lanName.indexOf("Wired connection") != -1) {
        QStringList strList = lanName.split(" ");
        if (strList.size() >= 3 && strList.at(2) != "") {
            returnLanName = tr("Wired connection") + " " + strList.at(2);
        }
    }

    if (lanName == "以太网连接") {
        returnLanName = tr("Ethernet connection");
    } else if (lanName.indexOf("以太网连接") != -1) {
        QStringList strList = lanName.split(" ");
        if (strList.size() >= 2 && strList.at(1) != "") {
            returnLanName = tr("Ethernet connection") + " " + strList.at(1);
        }
    }

    if (lanName == "Ethernet connection") {
        returnLanName = tr("Ethernet connection");
    } else if (lanName.indexOf("Ethernet connection") != -1) {
        QStringList strList = lanName.split(" ");
        if (strList.size() >= 3 && strList.at(2) != "") {
            returnLanName = tr("Ethernet connection") + " " + strList.at(2);
        }
    }

    return returnLanName;
}

//使用uuid获取对应的mac地址
QString MainWindow::getMacByUuid(QString uuidName)
{
    QString resultMac = "";

    QString tmpPath = "/tmp/kylin-nm-lanprop-" + QDir::home().dirName();
    QString cmd = "nmcli connection show '" + uuidName + "' > " + tmpPath;
    Utils::m_system(cmd.toUtf8().data());

    QFile file(tmpPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        syslog(LOG_ERR, "Can't open the file /tmp/kylin-nm-lanprop!");
        qDebug()<<"Can't open the file /tmp/kylin-nm-lanprop!"<<endl;
    }

    QString txt = file.readAll();
    QStringList txtLine = txt.split("\n");
    file.close();

    foreach (QString line, txtLine) {
        if (line.startsWith("802-3-ethernet.mac-address:")) {
            resultMac = line.mid(30).trimmed();
        }
    }

    return resultMac;
}

void MainWindow::checkIfConnectedWifiExist()
{
    //在点击托盘图标时，判读有无wifi的连接，若wstate不为0，但是有wifi连接，就断开
    BackThread *bt = new BackThread();
    IFace *iface = bt->execGetIface();
    if (iface->wstate != 0) {
        QString toDisConnWifi = "nmcli connection down '" + actWifiUuid + "'";
        system(toDisConnWifi.toUtf8().data());
    }
}

///////////////////////////////////////////////////////////////////////////////
//主窗口其他按钮点击响应

void MainWindow::on_btnAdvConf_clicked()
{
//    if (!kysec_is_disabled() && kysec_get_3adm_status()) {
//        //三权分立启用的操作
//        qDebug() << "三权分立启用的操作------>";
//        QMessageBox msgBox;
//        msgBox.setWindowTitle(QObject::tr("kylin-nm"));
//        msgBox.setIcon(QMessageBox::Warning);
//        msgBox.setText(QObject::tr("Insufficient permissions"));
//        msgBox.addButton(QMessageBox::Ok);
//        msgBox.button(QMessageBox::Ok)->setText(QObject::tr("OK"));
//        msgBox.exec();
//    } else {
//        //三权分立未启用的操作
//        qDebug() << "三权分立未启用的操作";
//        QProcess *qprocess = new QProcess(this);
//        qprocess->start("nm-connection-editor &");
//    }
    QProcess p(0);
    p.startDetached("nm-connection-editor &");
    p.waitForStarted();
    return;
}

void MainWindow::on_btnFlyMode_clicked()
{
    if (is_fly_mode_on == 0) {
        is_fly_mode_on = 1;

        onBtnWifiClicked(0);
        on_btnWifiList_clicked();
    } else {
        is_fly_mode_on = 0;
    }
}

void MainWindow::on_btnHotspot_clicked()
{
    if (is_wireless_adapter_ready == 1) {
        if (is_hot_sopt_on == 0) {
            ui->lbHotImg->setStyleSheet("QLabel{background-image:url(:/res/x/hot-spot-on.svg);}");
            ui->lbHotBG->setStyleSheet(btnOnQss);
            is_hot_sopt_on = 1;

            QApplication::setQuitOnLastWindowClosed(false);
            DlgHotspotCreate *hotCreate = new DlgHotspotCreate(objKyDBus->dbusWiFiCardName);
            connect(hotCreate,SIGNAL(updateHotspotList()),this,SLOT(on_btnWifiList_clicked() ));
            connect(hotCreate,SIGNAL(btnHotspotState()),this,SLOT(on_btnHotspotState() ));
            hotCreate->show();
        } else {
            on_btnHotspotState();

            BackThread objBT;
            objBT.disConnLanOrWifi("wifi");

            sleep(2);
            on_btnWifiList_clicked();
        }
    }
}

void MainWindow::onBtnAddNetClicked()
{
    QApplication::setQuitOnLastWindowClosed(false);
    DlgHideWifi *connHidWifi = new DlgHideWifi(0, this);
    connect(connHidWifi, SIGNAL(reSetWifiList() ), this, SLOT(on_btnWifiList_clicked()) );
    connHidWifi->show();
}

void MainWindow::onBtnCreateNetClicked()
{
    QPoint pos = QCursor::pos();
    QRect primaryGeometry;
    for (QScreen *screen : qApp->screens()) {
        if (screen->geometry().contains(pos)) {
            primaryGeometry = screen->geometry();
        }
    }

    if (primaryGeometry.isEmpty()) {
        primaryGeometry = qApp->primaryScreen()->geometry();
    }

    QApplication::setQuitOnLastWindowClosed(false);
    ConfForm *m_cf = new ConfForm();
    m_cf->cbTypeChanged(3);
    m_cf->move(primaryGeometry.width() / 2 - m_cf->width() / 2, primaryGeometry.height() / 2 - m_cf->height() / 2);
    m_cf->show();
}

/* 右键菜单打开网络设置界面 */
void MainWindow::actionTriggerSlots()
{
    this->move(-500, -500);
    this->showNormal();
    this->raise();
    this->activateWindow();
    QProcess p(0);
    p.startDetached("nm-connection-editor &");
    p.waitForStarted();
    return;
}

///////////////////////////////////////////////////////////////////////////////
//处理窗口变化、网络状态变化

//列表中item的扩展与收缩
void MainWindow::oneLanFormSelected(QString lanName, QString uniqueName)
{
    QList<OneLancForm *> topLanList = topLanListWidget->findChildren<OneLancForm *>();
    QList<OneLancForm *> lanList = lanListWidget->findChildren<OneLancForm *>();

    //**********************先处理下方列表********************//
    // 下方所有元素回到原位
    for (int i = 0, j = 0;i < lanList.size(); i ++) {
        OneLancForm *ocf = lanList.at(i);
        if (ocf->isActive == true) {
            ocf->move(L_VERTICAL_LINE_TO_ITEM, 0);
        }
        if (ocf->isActive == false) {
            ocf->move(L_VERTICAL_LINE_TO_ITEM, j * H_NORMAL_ITEM);
            j ++;
        }
    }

    //是否与上一次选中同一个网络框
    if (currSelNetName == uniqueName) {
        // 缩小所有选项卡
        for (int i = 0;i < lanList.size(); i ++) {
            OneLancForm *ocf = lanList.at(i);
            if (ocf->uuidName == uniqueName) {
                ocf->setSelected(false, true);
            } else {
                ocf->setSelected(false, false);
            }
        }

        currSelNetName = "";
    } else {
        int selectY = 0;
        for (int i = 0;i < lanList.size(); i ++) {
            OneLancForm *ocf = lanList.at(i);
            if (ocf->uuidName == uniqueName) {
                selectY = ocf->y(); //获取选中item的y坐标
                break;
            }
        }

        // 选中元素下面的所有元素下移 H_LAN_ITEM_EXTEND
        for (int i = 0;i < lanList.size(); i ++) {
            OneLancForm *ocf = lanList.at(i);
            if (ocf->y() > selectY) {
                ocf->move(L_VERTICAL_LINE_TO_ITEM, ocf->y() + H_LAN_ITEM_EXTEND);
            }
        }

        for (int i = 0;i < lanList.size(); i ++) {
            OneLancForm *ocf = lanList.at(i);
            if (ocf->uuidName == uniqueName) {
                ocf->setSelected(true, false);
                selectY = ocf->y();
            } else {
                ocf->setSelected(false, false);
            }
        }

        currSelNetName = uniqueName;
    }

    QList<OneLancForm *> itemList = lanListWidget->findChildren<OneLancForm *>();
    int n = itemList.size();
    if (n >= 1) {
        OneLancForm *lastItem = itemList.at(n-1);
        lastItem->setLine(false);
    }

    //**********************处理上方列表-界面所有控件回原位********************//
    topLanListWidget->resize(W_TOP_LIST_WIDGET, H_NORMAL_ITEM*currTopLanItem + H_GAP_UP + X_ITEM); // 顶部的item缩小
    lbTopLanList->move(X_MIDDLE_WORD, H_NORMAL_ITEM*currTopLanItem + H_GAP_UP);
    btnCreateNet->move(X_BTN_FUN, Y_BTN_FUN + H_NORMAL_ITEM*(currTopLanItem-1));
    scrollAreal->move(W_LEFT_AREA, Y_SCROLL_AREA + H_NORMAL_ITEM*(currTopLanItem-1));
    lbNoItemTip->move(this->width()/2 - W_NO_ITEM_TIP/2 + W_LEFT_AREA/2, this->height()/2 + H_NORMAL_ITEM*(currTopLanItem-1)/2);

    for (int i = topLanList.size() - 1;i >= 0; i --) {
        OneLancForm *ocf = topLanList.at(i);
        ocf->setTopItem(false);
        ocf->move(L_VERTICAL_LINE_TO_ITEM, (topLanList.size() - 1 - i) * H_NORMAL_ITEM);
    }
}
void MainWindow::oneTopLanFormSelected(QString lanName, QString uniqueName)
{
    QList<OneLancForm *> topLanList = topLanListWidget->findChildren<OneLancForm *>();
    QList<OneLancForm *> lanList = lanListWidget->findChildren<OneLancForm *>();

    // 顶部所有元素回到原位
    for (int i = topLanList.size() - 1;i >= 0; i --) {
        OneLancForm *ocf = topLanList.at(i);
        ocf->move(L_VERTICAL_LINE_TO_ITEM, (topLanList.size() - 1 - i) * H_NORMAL_ITEM);
    }

    if (currSelNetName == uniqueName) {
        // 与上一次选中同一个网络框，缩小当前选项卡
        topLanListWidget->resize(W_TOP_LIST_WIDGET, H_NORMAL_ITEM*currTopLanItem + H_GAP_UP + X_ITEM);
        lbTopLanList->move(X_MIDDLE_WORD, H_NORMAL_ITEM*currTopLanItem + H_GAP_UP);
        btnCreateNet->move(X_BTN_FUN, Y_BTN_FUN + H_NORMAL_ITEM*(currTopLanItem-1));
        scrollAreal->move(W_LEFT_AREA, Y_SCROLL_AREA + H_NORMAL_ITEM*(currTopLanItem-1));
        lbNoItemTip->move(this->width()/2 - W_NO_ITEM_TIP/2 + W_LEFT_AREA/2, this->height()/2 + H_NORMAL_ITEM*(currTopLanItem-1)/2);

        foreach (OneLancForm *ocf, topLanList) {
            ocf->setTopItem(false);
        }

        currSelNetName = "";
    } else {
        // 没有与上一次选中同一个网络框，放大当前选项卡

        //下方列表所有元素缩小并回到原位
        for (int i = 0; i < lanList.size(); i ++) {
            OneLancForm *ocf = lanList.at(i);
            ocf->setSelected(false, false);
            ocf->move(L_VERTICAL_LINE_TO_ITEM, i*H_NORMAL_ITEM);
        }

        topLanListWidget->resize(W_TOP_LIST_WIDGET, H_NORMAL_ITEM*currTopLanItem + H_LAN_ITEM_EXTEND + H_GAP_UP + X_ITEM);
        lbTopLanList->move(X_MIDDLE_WORD, H_NORMAL_ITEM*currTopLanItem + H_LAN_ITEM_EXTEND + H_GAP_UP);
        btnCreateNet->move(X_BTN_FUN, Y_BTN_FUN + H_LAN_ITEM_EXTEND + H_NORMAL_ITEM*(currTopLanItem-1));
        scrollAreal->move(W_LEFT_AREA, Y_SCROLL_AREA + H_LAN_ITEM_EXTEND + H_NORMAL_ITEM*(currTopLanItem-1));
        lbNoItemTip->move(this->width()/2 - W_NO_ITEM_TIP/2 + W_LEFT_AREA/2, this->height()/2 + 80 +  + H_NORMAL_ITEM*(currTopLanItem-1)/2);

        int selectY = 0;
        for (int i = 0;i < topLanList.size(); i ++) {
            OneLancForm *ocf = topLanList.at(i);
            if (ocf->uuidName == uniqueName) {
                selectY = ocf->y(); //获取选中item的y坐标
                break;
            }
        }

        // 选中元素下面的所有元素下移 H_LAN_ITEM_EXTEND
        for (int i = 0;i < topLanList.size(); i ++) {
            OneLancForm *ocf = topLanList.at(i);
            if (ocf->y() > selectY) {
                ocf->move(L_VERTICAL_LINE_TO_ITEM, ocf->y() + H_LAN_ITEM_EXTEND);
            }
        }

        //选中的元素进行扩展
        for (int i = 0;i < topLanList.size(); i ++) {
            OneLancForm *ocf = topLanList.at(i);
            if (ocf->uuidName == uniqueName) {
                ocf->setTopItem(true);
                selectY = ocf->y();
            } else {
                ocf->setTopItem(false);
            }
        }

        currSelNetName = uniqueName;
    }
}

void MainWindow::oneWifiFormSelected(QString wifibssid, int extendLength)
{
    QList<OneConnForm *>topWifiList = topWifiListWidget->findChildren<OneConnForm *>();
    QList<OneConnForm *> wifiList = wifiListWidget->findChildren<OneConnForm *>();

    //******************先处理下方列表****************//
    // 下方所有元素回到原位
    for (int i = 0, j = 0;i < wifiList.size(); i ++) {
        OneConnForm *ocf = wifiList.at(i);
        if (ocf->isActive == true) {
            ocf->move(L_VERTICAL_LINE_TO_ITEM, 0);
        }
        if (ocf->isActive == false) {
            ocf->move(L_VERTICAL_LINE_TO_ITEM, j * H_NORMAL_ITEM);
            j ++;
        }
    }

    //是否与上一次选中同一个网络框
    if (currSelNetName == wifibssid) {
        // 缩小所有选项卡
        for (int i = 0;i < wifiList.size(); i ++) {
            OneConnForm *ocf = wifiList.at(i);
            if (ocf->wifiBSsid == wifibssid) {
                if (ocf->wifiBSsid == hideWiFiConn) {
                    ocf->setHideItem(true, true);
                } else {
                    ocf->setSelected(false, true);
                }
            } else {
                if (ocf->wifiBSsid == hideWiFiConn) {
                    ocf->setHideItem(true, true);
                } else {
                    ocf->setSelected(false, false);
                }
            }

        }
        currSelNetName = "";
    } else {
        int selectY = 0;
        for (int i = 0;i < wifiList.size(); i ++) {
            OneConnForm *ocf = wifiList.at(i);
            if (ocf->wifiBSsid == wifibssid) {
                selectY = ocf->y(); //获取选中item的y坐标
                break;
            }
        }

        // 选中元素下面的所有元素下移 H_WIFI_ITEM_BIG_EXTEND
        for (int i = 0;i < wifiList.size(); i ++) {
            OneConnForm *ocf = wifiList.at(i);
            if (ocf->y() > selectY) {
                ocf->move(L_VERTICAL_LINE_TO_ITEM, ocf->y() + extendLength);
            }
        }

        for (int i = 0;i < wifiList.size(); i ++) {
            OneConnForm *ocf = wifiList.at(i);
            if (ocf->wifiBSsid == wifibssid) {
                if (ocf->wifiBSsid == hideWiFiConn) {
                    ocf->setHideItem(true, true);
                } else {
                    ocf->setSelected(true, false);
                }
            } else {
                if (ocf->wifiBSsid == hideWiFiConn) {
                    ocf->setHideItem(true, true);
                } else {
                    ocf->setSelected(false, false);
                }
            }
        }

        currSelNetName = wifibssid;
    }

    //最后一个item没有下划线
    QList<OneConnForm *> itemList = wifiListWidget->findChildren<OneConnForm *>();
    int n = itemList.size();
    if (n >= 1) {
        OneConnForm *lastItem = itemList.at(n-1);
        lastItem->setLine(false);
    }

    //********************处理上方列表-界面所有控件回原位******************//
    // 顶部的item缩小
    topWifiListWidget->resize(W_TOP_LIST_WIDGET, H_NORMAL_ITEM + H_GAP_UP + X_ITEM);
    lbTopWifiList->move(X_MIDDLE_WORD, H_NORMAL_ITEM + H_GAP_UP);
    btnAddNet->move(X_BTN_FUN, Y_BTN_FUN);
    scrollAreaw->move(W_LEFT_AREA, Y_SCROLL_AREA);
    lbNoItemTip->move(this->width()/2 - W_NO_ITEM_TIP/2 + W_LEFT_AREA/2, this->height()/2);

    OneConnForm *ocf = topWifiList.at(0);
    ocf->setTopItem(false);
}
void MainWindow::oneTopWifiFormSelected(QString wifibssid, int extendLength)
{
    QList<OneConnForm *>topWifiList = topWifiListWidget->findChildren<OneConnForm *>();
    QList<OneConnForm *> wifiList = wifiListWidget->findChildren<OneConnForm *>();

    if (currSelNetName == wifibssid) {
        // 与上一次选中同一个网络框，缩小当前选项卡
        topWifiListWidget->resize(W_TOP_LIST_WIDGET, H_NORMAL_ITEM + H_GAP_UP + X_ITEM);
        lbTopWifiList->move(X_MIDDLE_WORD, H_NORMAL_ITEM + H_GAP_UP);
        btnAddNet->move(X_BTN_FUN, Y_BTN_FUN);
        scrollAreaw->move(W_LEFT_AREA, Y_SCROLL_AREA);
        lbNoItemTip->move(this->width()/2 - W_NO_ITEM_TIP/2 + W_LEFT_AREA/2, this->height()/2);

        OneConnForm *ocf = topWifiList.at(0);
        ocf->setTopItem(false);

        currSelNetName = "";
    } else {
        // 没有与上一次选中同一个网络框，放大当前选项卡

        for(int i = 0;i < wifiList.size(); i ++) {
            // 所有元素回到原位
            OneConnForm *ocf = wifiList.at(i);
            ocf->setSelected(false, false);
            ocf->move(L_VERTICAL_LINE_TO_ITEM, i * H_NORMAL_ITEM);
        }

        topWifiListWidget->resize(W_TOP_LIST_WIDGET, H_NORMAL_ITEM + H_WIFI_ITEM_BIG_EXTEND + H_GAP_UP + X_ITEM);
        lbTopWifiList->move(X_MIDDLE_WORD, H_NORMAL_ITEM + H_WIFI_ITEM_BIG_EXTEND + H_GAP_UP);
        btnAddNet->move(X_BTN_FUN, Y_BTN_FUN + H_WIFI_ITEM_BIG_EXTEND);
        scrollAreaw->move(W_LEFT_AREA, Y_SCROLL_AREA + H_WIFI_ITEM_BIG_EXTEND);
        lbNoItemTip->move(this->width()/2 - W_NO_ITEM_TIP/2 + W_LEFT_AREA/2, this->height()/2 + 65);

        OneConnForm *ocf = topWifiList.at(0);
        ocf->setTopItem(true);

        currSelNetName = wifibssid;
    }
}

//断开网络处理
void MainWindow::activeLanDisconn()
{
    syslog(LOG_DEBUG, "Wired net is disconnected");

    QString txt(tr("Wired net is disconnected"));
    objKyDBus->showDesktopNotify(txt);
    currSelNetName = "";
    oldActLanName = "";
    oldDbusActLanDNS = 0;
    emit this->waitLanStop();
    this->ksnm->execGetLanList();
}

void MainWindow::activeWifiDisconn()
{
    hasWifiConnected = false;
    currSelNetName = "";
    this->ksnm->execGetWifiList(this->wcardname);
    QtConcurrent::run([=]() {
        activeStartLoading();
    });
}
void MainWindow::activeStartLoading()
{
    syslog(LOG_DEBUG, "Wi-Fi is disconnected");
    QString txt(tr("Wi-Fi is disconnected"));
    objKyDBus->showDesktopNotify(txt);

    setTrayLoading(false);
    getActiveInfoAndSetTrayIcon();
}

//网络开关处理，打开与关闭网络
void MainWindow::enNetDone()
{
    BackThread *bt = new BackThread();
    mwBandWidth = bt->execChkLanWidth(lcardname);

    // 打开网络开关时如果Wifi开关是打开的，设置其样式
    if (checkWlOn()) {
        btnWireless->setSwitchStatus(true);
    }

    onBtnNetListClicked(1);
    is_stop_check_net_state = 0;

    qDebug()<<"debug: already turn on the switch of lan network";
    syslog(LOG_DEBUG, "Already turn on the switch of lan network");
}
void MainWindow::disNetDone()
{
    this->is_btnLanList_clicked = 1;
    this->is_btnWifiList_clicked = 0;

    ui->lbNetListBG->setStyleSheet(btnOnQss);
    ui->lbWifiListBG->setStyleSheet(btnOffQss);

    ui->lbNetwork->setText(tr("Ethernet"));
    btnWireless->hide();

    delete topLanListWidget; // 清空top列表
    createTopLanUI(); //创建顶部有线网item

    // 清空lan列表
    lanListWidget = new QWidget(scrollAreal);
    lanListWidget->resize(W_LIST_WIDGET, H_NORMAL_ITEM + H_LAN_ITEM_EXTEND);
    scrollAreal->setWidget(lanListWidget);
    scrollAreal->move(W_LEFT_AREA, Y_SCROLL_AREA);

    // 当前连接的lan
    OneLancForm *ccf = new OneLancForm(topLanListWidget, this, confForm, ksnm);
    ccf->setName(tr("Not connected"), tr("Not connected"), "--", "--");//"当前未连接任何 以太网"
    ccf->setIcon(false);
    ccf->setConnedString(1, tr("Disconnected"), "");//"未连接"
    ccf->isConnected = false;
    ccf->setTopItem(false);
    ccf->setAct(true);
    ccf->move(L_VERTICAL_LINE_TO_ITEM, 0);
    ccf->show();

    btnWireless->setSwitchStatus(false);

    this->lanListWidget->show();
    this->wifiListWidget->hide();
    this->scrollAreal->show();
    this->topLanListWidget->show();
    this->scrollAreaw->hide();
    this->topWifiListWidget->hide();

    qDebug()<<"debug: already turn off the switch of lan network";
    syslog(LOG_DEBUG, "Already turn off the switch of lan network");

    this->stopLoading();
}

void MainWindow::enWifiDone()
{
    current_wifi_list_state = LOAD_WIFI_LIST;
    if (is_btnWifiList_clicked) {
        this->ksnm->execGetWifiList(this->wcardname);
    } else {
        //on_btnWifiList_clicked();
    }

    objKyDBus->getWirelessCardName();
    qDebug()<<"debug: already turn on the switch of wifi network";
    syslog(LOG_DEBUG, "Already turn on the switch of wifi network");
}
void MainWindow::disWifiDone()
{
    disWifiDoneChangeUI();

    qDebug()<<"debug: already turn off the switch of wifi network";
    syslog(LOG_DEBUG, "Already turn off the switch of wifi network");

    this->stopLoading();
    is_stop_check_net_state = 0;
}
void MainWindow::disWifiStateKeep()
{
    if (this->is_btnLanList_clicked == 1) {
        btnWireless->setSwitchStatus(false);
    }
    if (this->is_btnWifiList_clicked== 1) {
        disWifiDoneChangeUI();
        getActiveInfoAndSetTrayIcon();
    }
}
void MainWindow::disWifiDoneChangeUI()
{
    wifiListWidget = new QWidget(scrollAreaw);
    wifiListWidget->resize(W_LIST_WIDGET, H_WIFI_ITEM_BIG_EXTEND);
    scrollAreaw->setWidget(wifiListWidget);
    scrollAreaw->move(W_LEFT_AREA, Y_SCROLL_AREA);

    lbTopWifiList->move(X_MIDDLE_WORD, H_NORMAL_ITEM + H_GAP_UP);
    btnAddNet->move(X_BTN_FUN, Y_BTN_FUN);
    topWifiListWidget->resize(W_TOP_LIST_WIDGET, H_NORMAL_ITEM + H_GAP_UP + X_ITEM);

    QList<OneConnForm *> wifiList = topWifiListWidget->findChildren<OneConnForm *>();
    for (int i = 0; i < wifiList.size(); i ++) {
        OneConnForm *ocf = wifiList.at(i);
        if (ocf->isActive == true) {
            ocf->setSelected(false, false);
            ocf->setName(tr("Not connected"), "--", "--", "--");//"当前未连接任何 Wifi"
            ocf->setSignal("0", "--");
            ocf->setConnedString(1, tr("Disconnected"), "");//"未连接"
            ocf->lbFreq->hide();
            lbLoadDown->hide();
            lbLoadUp->hide();
            lbLoadDownImg->hide();
            lbLoadUpImg->hide();
            ocf->isConnected = false;
            ocf->setTopItem(false);
            disconnect(ocf, SIGNAL(selectedOneWifiForm(QString,int)), this, SLOT(oneTopWifiFormSelected(QString,int)));
        } else {
            ocf->deleteLater();
        }
    }

    btnWireless->setSwitchStatus(false);

    this->lanListWidget->hide();
    this->topLanListWidget->hide();
    this->wifiListWidget->show();
    this->topWifiListWidget->show();
    this->scrollAreal->hide();
    this->scrollAreaw->show();
}

void MainWindow::on_btnHotspotState()
{
    ui->lbHotImg->setStyleSheet("QLabel{background-image:url(:/res/x/hot-spot-off.svg);}");
    ui->lbHotBG->setStyleSheet(btnOffQss);
    is_hot_sopt_on = 0;
}

//执行wifi的重新连接
void MainWindow::toReconnectWifi()
{
    if (canReconnectWifiTimeInterval) {
        canReconnectWifiTimeInterval = false;
        QTimer::singleShot(2*1000, this, SLOT(timeIntervalToConnectWifi() ));

        if (isHuaWeiPC) {
            current_wifi_list_state = RECONNECT_WIFI;
            this->ksnm->execGetWifiList(this->wcardname);
        }
    }
}

void MainWindow::timeIntervalToConnectWifi()
{
    canReconnectWifiTimeInterval = true;
}

//处理外界对网络的连接与断开
void MainWindow::onExternalConnectionChange(QString type, bool isConnUp)
{
    isReconnectingWifi = false;

    if ( (type == "802-11-wireless" || type == "wifi") && !isConnUp ){
        //如果是手动点击断开连接，因为还在处理中，所以is_stop_check_net_state的值为true,
        //这样对于点击手动断开连接后立马发出的断开连接信号，便不会做处理
//        if (!is_stop_check_net_state) {
//            if (canReconnectWifiList.size() >= 1) {
//                int removePos = canReconnectWifiList.size() - 1;
//                //从列表中移除最后一个wifi，因为可能有外界因素导致wifi断开连接
//                canReconnectWifiList.removeAt(removePos);
//            }
//        }
        QTimer::singleShot(2*1000, this, SLOT(onToResetValue() ));
    }
    if (type == "802-11-wireless" || type == "wifi") {
        addNumberForWifi += 1;
    }
    if (addNumberForWifi == 2) {
        //断开一个wifi的时候，如果存在回连，可能接连发出两个信号
        //当连续发出wifi断开与连接的信号时，短时间内addNumberForWifi值为2
        is_stop_check_net_state = 1;
        if (is_connect_net_failed) {
            qDebug()<<"debug: connect wifi failed just now, no need to refresh wifi interface";
            is_connect_net_failed = 0;
            is_stop_check_net_state = 0;
        } else if (is_wifi_reconnected) {
            qDebug()<<"debug: wifi reconnected just now, no need to refresh wifi interface";
            is_wifi_reconnected = 0;
            is_stop_check_net_state = 0;
        }else {
            QTimer::singleShot(1*1000, this, SLOT(onExternalWifiChange() ));
        }
        addNumberForWifi = 0;
        return;
    }
    if ( (type == "802-11-wireless" || type == "wifi") && isConnUp ){
        addNumberForWifi = 0;
    }

    QTimer::singleShot(4*1000, this, SLOT(onToSetTrayIcon() ));

    if (type == "") {
        is_stop_check_net_state = 0;
        return;
    }

    if (isToSetLanValue) {
        if (type == "802-3-ethernet" || type == "ethernet") {
            isLanBeConnUp = true;
            isLanBeConnUp = isConnUp;
        }
    }

    if (isToSetWifiValue) {
        if (type == "802-11-wireless" || type == "wifi") {
            isWifiBeConnUp = true;
            isWifiBeConnUp = isConnUp;
        }
    }

    if (!is_connect_hide_wifi && !is_stop_check_net_state) {
        is_stop_check_net_state = 1;

        if (type == "802-3-ethernet" || type == "ethernet") {
            if (is_connect_net_failed) {
                qDebug()<<"debug: connect wired network failed, no need to refresh wired interface";
                is_connect_net_failed = 0;
                is_stop_check_net_state = 0;
            } else {
                isToSetLanValue = false;
                QTimer::singleShot(2*1000, this, SLOT(onExternalLanChange() ));
            }
        }

        if (type == "802-11-wireless" || type == "wifi") {
            addNumberForWifi = 0;
            if (is_connect_net_failed) {
                qDebug()<<"debug: connect wifi failed just now, no need to refresh wifi interface";
                is_connect_net_failed = 0;
                is_stop_check_net_state = 0;
            } else if (is_wifi_reconnected) {
                qDebug()<<"debug: wifi reconnected just now, no need to refresh wifi interface";
                is_wifi_reconnected = 0;
                is_stop_check_net_state = 0;
            }else {
                isToSetWifiValue = false;
                QTimer::singleShot(2*1000, this, SLOT(onExternalWifiChange() ));
            }
        }
    }
}

void MainWindow::onExternalLanChange()
{
    if (is_btnLanList_clicked) {
        onBtnNetListClicked(0);
    } else {
        is_stop_check_net_state = 0;
    }

    isToSetLanValue = true;
}

void MainWindow::onExternalWifiChange()
{
    if (!isWifiBeConnUp) {
        //QString txt(tr("WiFi already disconnect"));
        //objKyDBus->showDesktopNotify(txt);
    }
    if (isWifiBeConnUp) {
        //QString txt(tr("WiFi already connected external"));
        //objKyDBus->showDesktopNotify(txt);
    }

    if (is_btnWifiList_clicked) {
         on_btnWifiList_clicked();
    } else {
        is_stop_check_net_state = 0;
    }

    isToSetWifiValue = true;
}

void MainWindow::onToSetTrayIcon()
{
    getActiveInfoAndSetTrayIcon();
}

void MainWindow::onToResetValue()
{
    addNumberForWifi = 0;
}

void MainWindow::onWifiSwitchChange()
{
    if (is_btnWifiList_clicked) {
        //this->ksnm->execGetWifiList();
        on_btnWifiList_clicked();
    }
}

//处理外界对wifi开关的打开与关闭
void MainWindow::onExternalWifiSwitchChange(bool wifiEnabled)
{
    if (!is_stop_check_net_state) {
        is_stop_check_net_state = 1;
        if (wifiEnabled) {
            qDebug()<<"debug: external wifi switch turn on";
            syslog(LOG_DEBUG, "debug: external wifi switch turn on");
            QTimer::singleShot(4*1000, this, SLOT(onWifiSwitchChange() ));
            objKyDBus->setWifiSwitchState(true);
        } else {
            qDebug()<<"debug: external wifi switch turn off";
            syslog(LOG_DEBUG, "debug: external wifi switch turn off");
            QTimer::singleShot(3*1000, this, SLOT(onWifiSwitchChange() ));
            objKyDBus->setWifiSwitchState(false);//通知控制面板wifi开关已经关闭
        }
    }
}


///////////////////////////////////////////////////////////////////////////////
//循环处理部分，目前仅on_checkWifiListChanged 与on_setNetSpeed两个函数在运行

void MainWindow::onRequestScanAccesspoint()
{
    if (isHuaWeiPC) {
        numberForWifiScan += 1;
        int numScanPerSecond0 = 0;
        int numScanPerSecond1 = 0;
        if (numberForWifiScan > 12) {
            numScanPerSecond0 = (numberForWifiScan-12)/6; //获取除数
            numScanPerSecond1 = (numberForWifiScan-12)%6; //获取余数
        }

        if (is_init_wifi_list || is_connect_hide_wifi) {
            return; //遇到启动软件第一次加载wifi列表的时候，或正在连接隐藏wifi，停止更新
        }

        if (is_stop_check_net_state==0 && this->is_btnWifiList_clicked==1) {
            if (this->isVisible()) {
                this->toScanWifi(1);
            } else {
                switch (numberForWifiScan) {
                case 1 :
                    this->toScanWifi(0);
                    break;
                case 2:
                    this->toScanWifi(0);
                    break;
                case 3:
                    this->toScanWifi(0);
                    break;
                case 6:
                    this->toScanWifi(0);
                    break;
                case 9:
                    this->toScanWifi(0);
                    break;
                case 12:
                    this->toScanWifi(0);
                    break;
                }

                if ((numScanPerSecond0 != 0) && (numScanPerSecond1 == 0)) {
                    this->toScanWifi(0);
                }
            } // end if(this->isVisible())
        }

        if (numberForWifiScan > 500) numberForWifiScan = 0;
    } else {
        if (is_init_wifi_list || is_connect_hide_wifi) {
            return; //遇到启动软件第一次加载wifi列表的时候，或正在连接隐藏wifi，停止更新
        }

        if (is_stop_check_net_state==0 && this->is_btnWifiList_clicked==1 && this->isVisible()) {
            BackThread *loop_bt = new BackThread();
            IFace *loop_iface = loop_bt->execGetIface();

            if (loop_iface->wstate != 2) {
                current_wifi_list_state = UPDATE_WIFI_LIST;
                this->ksnm->execGetWifiList(this->wcardname); //更新wifi列表
            }

            delete loop_iface;
            loop_bt->deleteLater();
        }
    }
}

void MainWindow::toScanWifi(bool isShow)
{
    BackThread *loop_bt = new BackThread();
    IFace *loop_iface = loop_bt->execGetIface();

    if (loop_iface->wstate != 2) {
        if (loop_iface->wstate == 0) {
            if (isShow) {
                objKyDBus->requestScanWifi(); //要求后台扫描AP
            }
        }

        if (loop_iface->wstate == 1) {
            objKyDBus->requestScanWifi(); //要求后台扫描AP
        }

        if (isShow) {
            //只有在显示窗口时更新wifi列表
            current_wifi_list_state = UPDATE_WIFI_LIST;
            this->ksnm->execGetWifiList(this->wcardname); //更新wifi列表
        }
    }

    delete loop_iface;
    loop_bt->deleteLater();
}

void MainWindow::on_setNetSpeed()
{
    if (this->isVisible() && is_stop_check_net_state==0) {
        if (is_btnWifiList_clicked == 1) {
            if ( objNetSpeed->getCurrentDownloadRates(objKyDBus->dbusWiFiCardName.toUtf8().data(), &start_rcv_rates, &start_tx_rates) == -1) {
                start_rcv_rates = end_rcv_rates;
            }
        }
        else if(is_btnLanList_clicked == 1) {
            if ( objNetSpeed->getCurrentDownloadRates(currConnIfname.toUtf8().data(), &start_rcv_rates, &start_tx_rates) == -1) {
                start_tx_rates = end_tx_rates;
            }
        }
        long int delta_rcv = (start_rcv_rates - end_rcv_rates)/1024;
        long int delta_tx = (start_tx_rates - end_tx_rates)/1024;

        //简易滤波
        if (delta_rcv < 0 || delta_tx < 0) {
            delta_rcv = 0;
            delta_tx = 0;
        }
        else if (end_rcv_rates == 0 || end_tx_rates == 0){
            delta_rcv = 0;
            delta_tx = 0;
        }

        end_rcv_rates = start_rcv_rates;
        end_tx_rates = start_tx_rates;

        int rcv_num = delta_rcv;
        int tx_num = delta_tx;

        QString str_rcv;
        QString str_tx;

        if (rcv_num < 1024) {
            str_rcv = QString::number(rcv_num) + "KB/s.";
        } else {
            int remainder;
            if (rcv_num%1024 < 100) {
                remainder = 0;
            } else {
                remainder = (rcv_num%1024)/100;
            }
            str_rcv = QString::number(rcv_num/1024) + "."  + QString::number(remainder) + "MB/s.";
        }

        if (tx_num < 1024) {
            str_tx = QString::number(tx_num) + "KB/s";
        } else {
            int remainder;
            if (tx_num%1024 < 100) {
                remainder = 0;
            } else {
                remainder = (tx_num%1024)/100;
            }
            str_tx = QString::number(tx_num/1024) + "."  + QString::number(remainder) + "MB/s";
        }

        lbLoadDown->setText(str_rcv);
        lbLoadUp->setText(str_tx);

        int offset = str_rcv.length() * 7;
        lbLoadUp->move(X_ITEM + offset + 145, Y_TOP_ITEM + 32);
        lbLoadUpImg->move(X_ITEM + offset + 128, Y_TOP_ITEM + 35);
    }
    else {
        end_rcv_rates = 0;
        end_tx_rates = 0;
    }
}

void MainWindow::connLanDone(int connFlag)
{
    emit this->waitLanStop(); //停止加载动画

    // Lan连接结果，0点击连接成功 1因网线未插入失败 2因mac地址匹配不上失败 3开机启动网络工具时已经连接
    if (connFlag == 0) {
        syslog(LOG_DEBUG, "Wired net already connected by clicking button");
        //this->ksnm->execGetLanList();

        QString txt(tr("Conn Ethernet Success"));
        objKyDBus->showDesktopNotify(txt);
    }

    if (connFlag == 1) {
        syslog(LOG_DEBUG, "without net line connect to computer.");

        QString txt(tr("Without Lan Cable"));
        objKyDBus->showDesktopNotify(txt);
    }

    if (connFlag == 2) {
        this->ksnm->execGetLanList();
        QString txt(tr("Conn Ethernet Success"));
        objKyDBus->showDesktopNotify(txt);
    }

    if (connFlag == 3) {
        syslog(LOG_DEBUG, "Launch kylin-nm, Lan already connected");
    }

    if (connFlag == 4) {
        syslog(LOG_DEBUG, "IP configuration could not be reserved");
        is_connect_net_failed = 1;
        QString txt(tr("IP configuration could not be reserved"));
        objKyDBus->showDesktopNotify(txt);
    }

    if (connFlag == 5) {
        syslog(LOG_DEBUG, "The MACs of the device and the connection do not match.");
        is_connect_net_failed = 1;
        QString txt(tr("MAC Address Mismatch"));
        objKyDBus->showDesktopNotify(txt);
    }

    if (connFlag == 6) {
        syslog(LOG_DEBUG, "Connection Be Killed");
        is_connect_net_failed = 1;
        QString txt(tr("Connection Be Killed"));
        objKyDBus->showDesktopNotify(txt);
    }

    if (connFlag == 7) {
        syslog(LOG_DEBUG, "Connect Bluetooth Network Failed");
        is_connect_net_failed = 1;
        QString txt(tr("Connect Bluetooth Network Failed"));
        objKyDBus->showDesktopNotify(txt);
    }

    if (connFlag == 8) {
        syslog(LOG_DEBUG, "Carrier/link changed");
        is_connect_net_failed = 1;
        QString txt(tr("Carrier/link changed"));
        objKyDBus->showDesktopNotify(txt);
    }

    if (connFlag == 9) {
        syslog(LOG_DEBUG, "Connect Wired Network Failed");
        is_connect_net_failed = 1;
        QString txt(tr("Connect Wired Network Failed"));
        objKyDBus->showDesktopNotify(txt);
    }

    this->stopLoading();
    this->is_stop_check_net_state = 0;
}

void MainWindow::connWifiDone(int connFlag)
{
    // Wifi连接结果，0点击连接成功 1失败 2没有配置文件 3开机启动网络工具时已经连接 4密码错误 5找不到已保存的网络
    if (connFlag == 0) {
        syslog(LOG_DEBUG, "Wi-Fi already connected by clicking button");
        if (!isHuaWeiPC) {
            //如果不是华为电脑，使用获取连接信号的方式更新列表
            this->ksnm->execGetWifiList(this->wcardname);
        } else {
            //如果是华为电脑，连接wifi后判断到portal网络弹出认证框
            WifiAuthThread *wifi_auth_thread=new WifiAuthThread();
            wifi_auth_thread->start();
        }

        QString txt(tr("Conn Wifi Success"));
        objKyDBus->showDesktopNotify(txt);
    } else if (connFlag == 1) {
//        is_stop_check_net_state = 0;
        is_connect_net_failed = 1;
        QString txt(tr("Confirm your Wi-Fi password or usable of wireless card"));
        objKyDBus->showDesktopNotify(txt);
    } else if (connFlag == 3) {
        syslog(LOG_DEBUG, "Launch kylin-nm, Wi-Fi already connected");
    } else if (connFlag == 4) {
        is_connect_net_failed = 1;
        QString txt(tr("Confirm your Wi-Fi password"));
        objKyDBus->showDesktopNotify(txt);
    } else if (connFlag == 5) {
        is_connect_net_failed = 1;
        QString txt(tr("Selected Wifi has not been scanned."));
        objKyDBus->showDesktopNotify(txt);
    }
    this->stopLoading();
    is_stop_check_net_state = 0;
}

void MainWindow::onRequestRefreshWifiList()
{
    this->ksnm->execGetWifiList(this->wcardname);
}

//重新绘制背景色
void MainWindow::paintEvent(QPaintEvent *event)
{
    double trans = objKyDBus->getTransparentData();

    QStyleOption opt;
    opt.init(this);
    QPainter p(this);
    QRect rect = this->rect();
    rect.adjust(1, 1, -1, -1);
    p.setRenderHint(QPainter::Antialiasing);  // 反锯齿;
    p.setBrush(opt.palette.color(QPalette::Base));
    p.setOpacity(trans);
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(rect, 6, 6);
    QWidget::paintEvent(event);

    QPainterPath path;
    path.addRoundedRect(rect, 6, 6);
    setProperty("blurRegion", QRegion(path.toFillPolygon().toPolygon()));
    KWindowEffects::enableBlurBehind(this->winId(), true, QRegion(path.toFillPolygon().toPolygon()));
}

bool MainWindow::event(QEvent *event)
{
    if (event->type() == QEvent::ActivationChange) {
        if (QApplication::activeWindow() != this) {
            this->hide();
            numberForWifiScan = 0;
        }
    }

    return QMainWindow::event(event);
}

///////////////////////////////////////////////////////////////////////////////
QPixmap MainWindow::drawSymbolicColoredPixmap(const QPixmap &source)
{
    QColor gray(128,128,128);
    QColor standard (31,32,34);
    QImage img = source.toImage();
    for (int x = 0; x < img.width(); x++) {
        for (int y = 0; y < img.height(); y++) {
            auto color = img.pixelColor(x, y);
            if (color.alpha() > 0) {
                if (qAbs(color.red()-gray.red())<20 && qAbs(color.green()-gray.green())<20 && qAbs(color.blue()-gray.blue())<20) {
                    color.setRed(255);
                    color.setGreen(255);
                    color.setBlue(255);
                    img.setPixelColor(x, y, color);
                } else if(qAbs(color.red()-standard.red())<20 && qAbs(color.green()-standard.green())<20 && qAbs(color.blue()-standard.blue())<20) {
                    color.setRed(255);
                    color.setGreen(255);
                    color.setBlue(255);
                    img.setPixelColor(x, y, color);
                } else {
                    img.setPixelColor(x, y, color);
                }
            }
        }
    }
    return QPixmap::fromImage(img);
}

QPixmap MainWindow::drawSymbolicBlackColoredPixmap(const QPixmap &source)
{
    QImage img = source.toImage();
    for (int x = 0; x < img.width(); x++) {
        for (int y = 0; y < img.height(); y++) {
            auto color = img.pixelColor(x, y);
            if (color.alpha() > 0) {
                if (qAbs(color.red())>=200 && qAbs(color.green())>=200 && qAbs(color.blue())>=200) {
                    color.setRed(56);
                    color.setGreen(56);
                    color.setBlue(56);
                    img.setPixelColor(x, y, color);
                }
            }
        }
    }
    return QPixmap::fromImage(img);
}

const QPixmap MainWindow::loadSvg(const QString &fileName, const int size)
{
    QPixmap pixmap(size, size);
    QSvgRenderer renderer(fileName);
    pixmap.fill(Qt::transparent);

    QPainter painter;
    painter.begin(&pixmap);
    renderer.render(&painter);
    painter.end();

    return pixmap;
}

void MainWindow::getSystemFontFamily()
{
    const QByteArray id("org.ukui.style");
    QGSettings * fontSetting = new QGSettings(id, QByteArray(), this);
    connect(fontSetting, &QGSettings::changed,[=](QString key) {
        if ("systemFont" == key || "systemFontSize" ==key) {
            QFont font = this->font();
            int width = font.pointSize();
            for (auto widget : qApp->allWidgets()) {
                widget->setFont(font);
            }
        }
    });
}


///////////////////////////////////////////////////////////////////////////////
//解决双屏幕问题用到的dbus方法
void MainWindow::PrimaryManager()
{
    //QDBusConnection conn = QDBusConnection::sessionBus();
    mDbusXrandInter = new QDBusInterface(DBUS_NAME,
                                         DBUS_PATH,
                                         DBUS_INTERFACE,
                                         QDBusConnection::sessionBus());

    isHuaWeiPC = false;
    QDBusMessage message = QDBusMessage::createMethodCall(DBUS_NAME,
                               DBUS_PATH,
                               DBUS_INTERFACE,
                               "height");
    QDBusMessage response = QDBusConnection::sessionBus().call(message);
    if (response.type() == QDBusMessage::ReplyMessage) {
        isHuaWeiPC = true;
    }

    connect(mDbusXrandInter, SIGNAL(screenPrimaryChanged(int,int,int,int)),
            this, SLOT(priScreenChanged(int,int,int,int)));

}

void MainWindow::toStart()
{
    m_priX = getScreenGeometry("x");
    m_priY = getScreenGeometry("y");
    m_priWid = getScreenGeometry("width");
    m_priHei = getScreenGeometry("height");
    if (m_priWid == 0 || m_priHei == 0) {
        QRect rect = QApplication::desktop()->screenGeometry(0);
        m_priX=rect.x();
        m_priY=rect.y();
        m_priWid=rect.width();
        m_priHei=rect.height();
    }

    //qDebug("Start: Primary screen geometry is x=%d, y=%d, windth=%d, height=%d", m_priX, m_priY, m_priWid, m_priHei);
}

int MainWindow::getScreenGeometry(QString methodName)
{
    int res = 0;
    QDBusMessage message = QDBusMessage::createMethodCall(DBUS_NAME,
                               DBUS_PATH,
                               DBUS_INTERFACE,
                               methodName);
    QDBusMessage response = QDBusConnection::sessionBus().call(message);
    if (response.type() == QDBusMessage::ReplyMessage)
    {
        if(response.arguments().isEmpty() == false) {
            int value = response.arguments().takeFirst().toInt();
            res = value;
            qDebug() << value;
        }
    } else {
        qDebug()<< "to get message about "<< methodName<<" of screen failed";
    }
    return res;
}

/* get primary screen changed */
void MainWindow::priScreenChanged(int x, int y, int width, int height)
{
    m_priX = x;
    m_priY = y;
    m_priWid = width;
    m_priHei = height;
    qDebug("primary screen  changed, geometry is  x=%d, y=%d, windth=%d, height=%d", x, y, width, height);
}

// 通过kds的dbus发现rfkill状态变化
void MainWindow::onRfkillStatusChanged()
{
    QDBusReply<int> reply = kdsDbus->call("getCurrentWlanMode");
    if (reply.isValid()){
        int current = reply.value();

        if (current == -1){
            qWarning("Error Occur When Get Current WlanMode");
            return;
        }
        if (!current) {
            is_stop_check_net_state = 1;
            lbTopWifiList->hide();
            btnAddNet->hide();
            objKyDBus->setWifiSwitchState(false);

            QThread *t = new QThread();
            BackThread *bt = new BackThread();
            bt->moveToThread(t);
            btnWireless->setSwitchStatus(true);
            connect(t, SIGNAL(finished()), t, SLOT(deleteLater()));
            connect(t, SIGNAL(started()), bt, SLOT(execDisWifi()));
            connect(bt, SIGNAL(disWifiDone()), this, SLOT(rfkillDisableWifiDone()));
            connect(bt, SIGNAL(btFinish()), t, SLOT(quit()));
            t->start();
            this->startLoading();
        } else {
            if (is_fly_mode_on == 0) {
                on_btnWifiList_clicked();
                is_stop_check_net_state = 1;
                objKyDBus->setWifiCardState(true);
                objKyDBus->setWifiSwitchState(true);

                QThread *t = new QThread();
                BackThread *bt = new BackThread();
                bt->moveToThread(t);
                btnWireless->setSwitchStatus(true);
                connect(t, SIGNAL(finished()), t, SLOT(deleteLater()));
                connect(t, SIGNAL(started()), bt, SLOT(execEnWifi()));
                connect(bt, SIGNAL(enWifiDone()), this, SLOT(rfkillEnableWifiDone()));
                connect(bt, SIGNAL(btFinish()), t, SLOT(quit()));
                t->start();
                this->startLoading();
            }
        }
    }
}

void MainWindow::rfkillDisableWifiDone()
{
    if (is_btnWifiList_clicked)
        disWifiDoneChangeUI();

    qDebug()<<"debug: already turn off the switch of wifi network by keyboard button";
    syslog(LOG_DEBUG, "Already turn off the switch of wifi network by keyboard button");

    this->stopLoading();
    is_stop_check_net_state = 0;
}

void MainWindow::rfkillEnableWifiDone()
{
    current_wifi_list_state = LOAD_WIFI_LIST;
    if (is_btnWifiList_clicked) {
        this->ksnm->execGetWifiList(this->wcardname);
    } else {
//        on_btnWifiList_clicked();
    }

    objKyDBus->getWirelessCardName();
    qDebug()<<"debug: already turn on the switch of wifi network";
    syslog(LOG_DEBUG, "Already turn on the switch of wifi network");
}
