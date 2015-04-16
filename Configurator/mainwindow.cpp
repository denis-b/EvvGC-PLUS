#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QMessageBox>
#include <QtSerialPort/QSerialPortInfo>
#include <QDebug>

static PIDSettings PID[3] = {
/*   P, I, D, F */
    {0, 0, 0, 0},  /* Pitch */
    {0, 0, 0, 0},  /* Roll  */
    {0, 0, 0, 0}   /* Yaw   */
};

static OutputSettings outSettings[3] = {
    {0,     /* Power, %        */
     14,    /* Number of poles */
     0x00,  /* Flags           */
     0x53}, /* DTime-Cmd ID    */
    {0,     /* Power, %        */
     14,    /* Number of poles */
     0x00,  /* Flags           */
     0x53}, /* DTime-Cmd ID    */
    {0,     /* Power, %        */
     14,    /* Number of poles */
     0x00,  /* Flags           */
     0x53}  /* DTime-Cmd ID    */
};

static InputSettings inSettings[3] = {
    {1000,  /* Min       */
     1500,  /* Mid       */
     2000,  /* Max       */
     5},    /* Channel # */
    {1000,  /* Min       */
     1500,  /* Mid       */
     2000,  /* Max       */
     5},    /* Channel # */
    {1000,  /* Min       */
     1500,  /* Mid       */
     2000,  /* Max       */
     5}     /* Channel # */
};

static InputModeSettings modeSettings[3] = {
    {-60,   /* Min angle */
     60,    /* Max angle */
     0,     /* Offset    */
     20,    /* Speed     */
     0},    /* Mode ID   */
    {-60,   /* Min angle */
     60,    /* Max angle */
     0,     /* Offset    */
     20,    /* Speed     */
     0},    /* Mode ID   */
    {-90,   /* Min angle */
     90,    /* Max angle */
     0,     /* Offset    */
     20,    /* Speed     */
     0}     /* Mode ID   */
};

/* Default sensor settings.
 * Structure of the sensor settings is:
 * D2|2I2|1I2|0I2|D1|2I1|1I1|0I1
 * where Dx  - axis direction of sensor x;
 *       nIx - n-th bit of axis ID of sensor x.
 */
static quint8 sensorSettings[3] = {
    0x00 |
    SENSOR1_AXIS_DIR_POS |
    SENSOR2_AXIS_DIR_POS, /* Pitch (X) */
    0x11,                 /* Roll  (Y) */
    0x22                  /* Yaw   (Z) */
};

static quint8 sensorAxes[3] = {
    0x33, /* Right axis -X; */
    0x44, /* Front axis -Y; */
    0x22  /* Top axis   +Z; */
};

/* Axes mapping matrix. */
static quint8 amMtx[6][6] = {
//T:+X,+Y,+Z,-X,-Y,-Z  //R:
    {6, 5, 1, 6, 2, 4},//+X;
    {2, 6, 3, 5, 6, 0},//+Y;
    {4, 0, 6, 1, 3, 6},//+Z;
    {6, 2, 4, 6, 5, 1},//-X;
    {5, 6, 0, 2, 6, 3},//-Y;
    {1, 3, 6, 4, 0, 6},//-Z;
};

static I2CErrorStruct i2cErrorInfo = {0, 0};

/**
 * @brief Quaternion2RPY
 * @param q
 * @param rpy
 */
static inline void Quaternion2RPY(const float q[4], float rpy[3])
{
    float R13, R11, R12, R23, R33;
    float q2s = q[2]*q[2];

    R11 = 1.0f - 2.0f * (q2s + q[3]*q[3]);
    R12 = 2.0f * (q[0]*q[3] + q[1]*q[2]);
    R13 = 2.0f * (q[0]*q[2] - q[1]*q[3]);
    R23 = 2.0f * (q[0]*q[1] + q[2]*q[3]);
    R33 = 1.0f - 2.0f * (q2s + q[1]*q[1]);

    rpy[1] = asinf (R13);   // roll always between -pi/2 to pi/2
    rpy[2] = atan2f(R12, R11);
    rpy[0] = atan2f(R23, R33);

    //TODO: consider the cases where |R13| ~= 1, |roll| ~= pi/2
}

/**
 * @brief MainWindow::MainWindow
 * @param parent
 */
MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow),
    m_SerialDeviceList(new QComboBox),
    fConnected(false),
    boardStatus(0)
{
    ui->setupUi(this);

    m_SerialDeviceList->setMinimumWidth(250);
    m_SerialDeviceList->setEnabled(false);
    ui->mainToolBar->insertWidget(ui->actionConnect, m_SerialDeviceList);
    ui->mainToolBar->insertSeparator(ui->actionConnect);

    connect(ui->actionConnect, SIGNAL(triggered()), this, SLOT(SerialConnect()));
    connect(ui->actionRead, SIGNAL(triggered()), this, SLOT(HandleReadSettings()));
    connect(ui->actionSet, SIGNAL(triggered()), this, SLOT(HandleApplySettings()));
    connect(ui->actionSave, SIGNAL(triggered()), this, SLOT(HandleSaveSettings()));
    connect(ui->actionBootloader, SIGNAL(triggered()), this, SLOT(ToBootloader()));
    connect(ui->pushSensor1AccCalibrate, SIGNAL(clicked()), this, SLOT(HandleAccCalibrate()));
    connect(ui->pushSensor1GyroCalibrate, SIGNAL(clicked()), this, SLOT(HandleGyroCalibrate()));
    connect(&m_timer, SIGNAL(timeout()), this, SLOT(ProcessTimeout()));
    connect(&m_thread, SIGNAL(serialError(QString)), this, SLOT(SerialError(QString)));
    connect(&m_thread, SIGNAL(serialTimeout(QString)), this, SLOT(SerialTimeout(QString)));
    connect(&m_thread, SIGNAL(serialDataReady(TelemetryMessage)), this, SLOT(ProcessSerialMessages(TelemetryMessage)));

    FillPortsInfo();

    SetStabilizationSettings();
    SetOutputSettings();
    SetInputSettings();
    SetInputModeSettings();
    SetSensorSettings();

    ui->comboData->setCurrentIndex(0);

    ui->plotData->addGraph();
     /* line color red for first graph. */
    ui->plotData->graph(0)->setPen(QPen(Qt::red));
    ui->plotData->addGraph();
    /* line color green for second graph. */
    ui->plotData->graph(1)->setPen(QPen(Qt::green));
    ui->plotData->addGraph();
    /* line color blue for third graph. */
    ui->plotData->graph(2)->setPen(QPen(Qt::blue));

    ui->plotData->xAxis->setTickLabelType(QCPAxis::ltDateTime);
    ui->plotData->xAxis->setDateTimeFormat("hh:mm:ss");
    ui->plotData->xAxis->setAutoTickStep(false);
    ui->plotData->xAxis->setTickStep(2);
    ui->plotData->axisRect()->setupFullAxesBox();

    ui->plotData->yAxis->setLabel("Attitude, deg");

    /* make left and bottom axes transfer their ranges to right and top axes: */
    connect(ui->plotData->xAxis, SIGNAL(rangeChanged(QCPRange)),
            ui->plotData->xAxis2, SLOT(setRange(QCPRange)));
    connect(ui->plotData->yAxis, SIGNAL(rangeChanged(QCPRange)),
            ui->plotData->yAxis2, SLOT(setRange(QCPRange)));

    connect(ui->checkDataX, SIGNAL(clicked()), this, SLOT(HandleDataXClicked()));
    connect(ui->checkDataY, SIGNAL(clicked()), this, SLOT(HandleDataYClicked()));
    connect(ui->checkDataZ, SIGNAL(clicked()), this, SLOT(HandleDataZClicked()));
}

/**
 * @brief MainWindow::~MainWindow
 */
MainWindow::~MainWindow()
{
    if (fConnected) {
        SerialConnect();
    }
    delete ui;
}

/**
 * @brief MainWindow::FillPortsInfo
 */
void MainWindow::FillPortsInfo()
{
    m_SerialDeviceList->clear();
    if (QSerialPortInfo::availablePorts().count() == 0) {
        m_SerialDeviceList->addItem(tr("Serial port not detected"), "None");
    } else {
        foreach (const QSerialPortInfo &info, QSerialPortInfo::availablePorts()) {
            m_SerialDeviceList->addItem(info.description() + ' ' + '(' + info.portName() + ')', info.portName());
        }
        m_SerialDeviceList->setEnabled(true);
        ui->actionConnect->setEnabled(true);
    }
}

/**
 * @brief MainWindow::HandleConnection
 */
void MainWindow::SerialConnect()
{
    if (fConnected) {
        if (m_timer.isActive()) {
            m_timer.stop();
        }
        m_thread.disconnect();
        ui->actionConnect->setText(tr("Connect"));
        ui->actionRead->setEnabled(false);
        ui->actionSet->setEnabled(false);
        ui->actionSave->setEnabled(false);
        m_SerialDeviceList->setEnabled(true);
        ui->statusBar->showMessage(tr("Disconnected from: %1").arg(m_SerialDeviceList->currentText()));
        fConnected = false;
    } else {
        m_thread.connect(m_SerialDeviceList->currentData().toString());
        ui->actionConnect->setText(tr("Disconnect"));
        ui->actionRead->setEnabled(true);
        ui->actionSet->setEnabled(true);
        m_SerialDeviceList->setEnabled(false);
        boardStatus = 0;
        BoardStatusApply();
        ui->statusBar->showMessage(tr("Connected to: %1").arg(m_SerialDeviceList->currentText()));
        fConnected = true;
        m_timer.start(20);
    }
}

void MainWindow::SerialDataWrite(const TelemetryMessage &msg)
{
    QByteArray data;
    data.append((char *)&msg, msg.size - TELEMETRY_CRC_SIZE_BYTES);
    data.append((char *)&msg.crc, TELEMETRY_CRC_SIZE_BYTES);
    m_thread.write(data);
}

void MainWindow::SerialError(const QString &s)
{
    if (fConnected) {
        if (m_timer.isActive()) {
            m_timer.stop();
        }
        ui->actionConnect->setText(tr("Connect"));
        ui->actionRead->setEnabled(false);
        ui->actionSet->setEnabled(false);
        ui->actionSave->setEnabled(false);
        m_SerialDeviceList->setEnabled(true);
        fConnected = false;
        ui->statusBar->showMessage(s);
    }
}

void MainWindow::SerialTimeout(const QString &s)
{
    if (fConnected) {
        if (m_timer.isActive()) {
            m_timer.stop();
        }
        ui->actionConnect->setText(tr("Connect"));
        ui->actionRead->setEnabled(false);
        ui->actionSet->setEnabled(false);
        ui->actionSave->setEnabled(false);
        m_SerialDeviceList->setEnabled(true);
        fConnected = false;
        ui->statusBar->showMessage(s);
    }
}

/**
 * @brief  MainWindow::GetCRC32Checksum
 * @param  pHdr - pointer to data header structure.
 * @return crc32 checksum of zero-padded data buffer.
 */
quint32 MainWindow::GetCRC32Checksum(const TelemetryMessage &msg)
{
    size_t crc_length = (msg.size - TELEMETRY_CRC_SIZE_BYTES) / sizeof(quint32);
    if ((msg.size - TELEMETRY_CRC_SIZE_BYTES) % sizeof(quint32)) {
        crc_length++;
    }
    return crc32((const quint32 *)&msg, crc_length);
}

/**
 * @brief MainWindow::HandleReadSettings
 */
void MainWindow::HandleReadSettings()
{
    m_msg.sof    = TELEMETRY_MSG_SOF;
    m_msg.size   = TELEMETRY_MSG_SIZE_BYTES;
    m_msg.res    = 0;

    /* Get board status data. */
    m_msg.msg_id = 'b';
    m_msg.crc    = GetCRC32Checksum(m_msg);
    SerialDataWrite(m_msg);

    /* Get I2C error info data. */
    m_msg.msg_id = 'e';
    m_msg.crc    = GetCRC32Checksum(m_msg);
    SerialDataWrite(m_msg);

    /* Get stabilization settings. */
    m_msg.msg_id = 's';
    m_msg.crc    = GetCRC32Checksum(m_msg);
    SerialDataWrite(m_msg);

    /* Get output settings. */
    m_msg.msg_id = 'o';
    m_msg.crc    = GetCRC32Checksum(m_msg);
    SerialDataWrite(m_msg);

    /* Get input settings. */
    m_msg.msg_id = 'p';
    m_msg.crc    = GetCRC32Checksum(m_msg);
    SerialDataWrite(m_msg);

    /* Get input mode settings. */
    m_msg.msg_id = 'n';
    m_msg.crc    = GetCRC32Checksum(m_msg);
    SerialDataWrite(m_msg);

    /* Get sensor settings. */
    m_msg.msg_id = 'd';
    m_msg.crc    = GetCRC32Checksum(m_msg);
    SerialDataWrite(m_msg);
}

/**
 * @brief MainWindow::HandleApplySettings
 */
void MainWindow::HandleApplySettings()
{
    m_msg.sof = TELEMETRY_MSG_SOF;
    m_msg.res = 0;

    if (GetStabilizationSettings()) {
        /* Write stabilization settings; */
        qDebug() << "Pitch P:" << PID[0].P << "I:" << PID[0].I << "D:" << PID[0].D << "F:" << PID[0].F;
        qDebug() << "Roll  P:" << PID[1].P << "I:" << PID[1].I << "D:" << PID[1].D << "F:" << PID[1].F;
        qDebug() << "Yaw   P:" << PID[2].P << "I:" << PID[2].I << "D:" << PID[2].D << "F:" << PID[2].F;
        qDebug() << "Size:" << sizeof(PID);

        /* Clean data buffer for zero-padded crc32 checksum calculation. */
        memset((void *)m_msg.data, 0, TELEMETRY_BUFFER_SIZE);
        memcpy((void *)m_msg.data, (void *)PID, sizeof(PID));
        m_msg.msg_id = 'S';
        m_msg.size   = sizeof(PID) + TELEMETRY_MSG_SIZE_BYTES;
        m_msg.crc    = GetCRC32Checksum(m_msg);

        SerialDataWrite(m_msg);
    }

    if (GetOutputSettings()) {
        /* Write output settings; */
        qDebug() << "Pitch Pwr:" << outSettings[0].power << "NPoles:" << outSettings[0].num_poles \
                 << "Rev:" << ((outSettings[0].flags & PWM_OUT_REV_FLAG) > 0) << "THI:" << ((outSettings[0].flags & PWM_OUT_THI_FLAG) > 0) \
                 << "Cmd:" << (outSettings[0].dt_cmd_id & PWM_OUT_CMD_ID_MASK) << "DT:" << (outSettings[0].dt_cmd_id >> 4);
        qDebug() << "Roll  Pwr:" << outSettings[1].power << "NPoles:" << outSettings[1].num_poles \
                 << "Rev:" << ((outSettings[1].flags & PWM_OUT_REV_FLAG) > 0) << "THI:" << ((outSettings[1].flags & PWM_OUT_THI_FLAG) > 0) \
                 << "Cmd:" << (outSettings[1].dt_cmd_id & PWM_OUT_CMD_ID_MASK) << "DT:" << (outSettings[1].dt_cmd_id >> 4);
        qDebug() << "Yaw   Pwr:" << outSettings[2].power << "NPoles:" << outSettings[2].num_poles \
                 << "Rev:" << ((outSettings[2].flags & PWM_OUT_REV_FLAG) > 0) << "THI:" << ((outSettings[2].flags & PWM_OUT_THI_FLAG) > 0) \
                 << "Cmd:" << (outSettings[2].dt_cmd_id & PWM_OUT_CMD_ID_MASK) << "DT:" << (outSettings[2].dt_cmd_id >> 4);
        qDebug() << "Size:" << sizeof(outSettings);

        /* Clean data buffer for zero-padded crc32 checksum calculation. */
        memset((void *)m_msg.data, 0, TELEMETRY_BUFFER_SIZE);
        memcpy((void *)m_msg.data, (void *)outSettings, sizeof(outSettings));
        m_msg.msg_id = 'O';
        m_msg.size   = sizeof(outSettings) + TELEMETRY_MSG_SIZE_BYTES;
        m_msg.crc    = GetCRC32Checksum(m_msg);

        SerialDataWrite(m_msg);
    }

    if (GetInputSettings()) {
        /* Write input settings; */
        qDebug() << "Pitch Ch#:" << inSettings[0].channel_id << "Min:" << inSettings[0].min_val \
                 << "Mid:" << inSettings[0].mid_val << "Max:" << inSettings[0].max_val;
        qDebug() << "Roll  Ch#:" << inSettings[1].channel_id << "Min:" << inSettings[1].min_val \
                 << "Mid:" << inSettings[1].mid_val << "Max:" << inSettings[1].max_val;
        qDebug() << "Yaw   Ch#:" << inSettings[2].channel_id << "Min:" << inSettings[2].min_val \
                 << "Mid:" << inSettings[2].mid_val << "Max:" << inSettings[2].max_val;
        qDebug() << "Size:" << sizeof(inSettings);

        /* Clean data buffer for zero-padded crc32 checksum calculation. */
        memset((void *)m_msg.data, 0, TELEMETRY_BUFFER_SIZE);
        memcpy((void *)m_msg.data, (void *)inSettings, sizeof(inSettings));
        m_msg.msg_id = 'I';
        m_msg.size   = sizeof(inSettings) + TELEMETRY_MSG_SIZE_BYTES;
        m_msg.crc    = GetCRC32Checksum(m_msg);

        SerialDataWrite(m_msg);
    }

    if (GetInputModeSettings()) {
        /* Write input mode settings; */
        qDebug() << "Pitch Mode#:" << modeSettings[0].mode_id << "MinA:" << modeSettings[0].min_angle \
                 << "MaxA:" << modeSettings[0].max_angle << "Speed:" << modeSettings[0].speed \
                 << "Offset:" << modeSettings[0].offset;
        qDebug() << "Roll  Mode#:" << modeSettings[1].mode_id << "MinA:" << modeSettings[1].min_angle \
                 << "MaxA:" << modeSettings[1].max_angle << "Speed:" << modeSettings[1].speed \
                 << "Offset:" << modeSettings[1].offset;
        qDebug() << "Yaw   Mode#:" << modeSettings[2].mode_id << "MinA:" << modeSettings[2].min_angle \
                 << "MaxA:" << modeSettings[2].max_angle << "Speed:" << modeSettings[2].speed \
                 << "Offset:" << modeSettings[2].offset;
        qDebug() << "Size:" << sizeof(modeSettings);

        /* Clean data buffer for zero-padded crc32 checksum calculation. */
        memset((void *)m_msg.data, 0, TELEMETRY_BUFFER_SIZE);
        memcpy((void *)m_msg.data, (void *)modeSettings, sizeof(modeSettings));
        m_msg.msg_id = 'N';
        m_msg.size   = sizeof(modeSettings) + TELEMETRY_MSG_SIZE_BYTES;
        m_msg.crc    = GetCRC32Checksum(m_msg);

        SerialDataWrite(m_msg);
    }

    if (GetSensorSettings()) {
        /* Write sensor settings; */
        qDebug() << "S1 FRONT id:" << (sensorSettings[0] & SENSOR1_AXIS_ID_MASK)\
                 << "Positive:" << ((sensorSettings[0] & SENSOR1_AXIS_DIR_POS) > 0);
        qDebug() << "S1 RIGHT id:" << (sensorSettings[1] & SENSOR1_AXIS_ID_MASK)\
                 << "Positive:" << ((sensorSettings[1] & SENSOR1_AXIS_DIR_POS) > 0);
        qDebug() << "S1 TOP   id:" << (sensorSettings[2] & SENSOR1_AXIS_ID_MASK)\
                 << "Positive:" << ((sensorSettings[2] & SENSOR1_AXIS_DIR_POS) > 0);
        qDebug() << "S2 FRONT id:" << ((sensorSettings[0] & SENSOR2_AXIS_ID_MASK) >> 4)\
                 << "Positive:" << ((sensorSettings[0] & SENSOR2_AXIS_DIR_POS) > 0);
        qDebug() << "S2 RIGHT id:" << ((sensorSettings[1] & SENSOR2_AXIS_ID_MASK) >> 4)\
                 << "Positive:" << ((sensorSettings[1] & SENSOR2_AXIS_DIR_POS) > 0);
        qDebug() << "S2 TOP   id:" << ((sensorSettings[2] & SENSOR2_AXIS_ID_MASK) >> 4)\
                 << "Positive:" << ((sensorSettings[2] & SENSOR2_AXIS_DIR_POS) > 0);
        qDebug() << "Size:" << sizeof(sensorSettings);

        /* Clean data buffer for zero-padded crc32 checksum calculation. */
        memset((void *)m_msg.data, 0, TELEMETRY_BUFFER_SIZE);
        memcpy((void *)m_msg.data, (void *)sensorSettings, sizeof(sensorSettings));
        m_msg.msg_id = 'D';
        m_msg.size   = sizeof(sensorSettings) + TELEMETRY_MSG_SIZE_BYTES;
        m_msg.crc    = GetCRC32Checksum(m_msg);

        SerialDataWrite(m_msg);
    }
}

/**
 * @brief MainWindow::HandleSaveSettings
 */
void MainWindow::HandleSaveSettings()
{
    HandleApplySettings();

    m_msg.sof  = TELEMETRY_MSG_SOF;
    m_msg.size = TELEMETRY_MSG_SIZE_BYTES;
    m_msg.res  = 0;

    /* Save to EEPROM. */
    m_msg.msg_id = 'c';
    m_msg.crc    = GetCRC32Checksum(m_msg);
    SerialDataWrite(m_msg);
}

/**
 * @brief MainWindow::ToBootloader
 */
void MainWindow::ToBootloader()
{
    m_msg.sof  = TELEMETRY_MSG_SOF;
    m_msg.size = TELEMETRY_MSG_SIZE_BYTES + 1;
    m_msg.res  = 0;

    /* Clean data buffer for zero-padded crc32 checksum calculation. */
    memset((void *)m_msg.data, 0, TELEMETRY_BUFFER_SIZE);

    /* Reset the board and enter bootloader. */
    m_msg.msg_id  = 'X';
    m_msg.data[0] = 1;
    m_msg.crc     = GetCRC32Checksum(m_msg);
    SerialDataWrite(m_msg);
}

/**
 * @brief MainWindow::ProcessTimeout
 */
void MainWindow::ProcessTimeout()
{
    m_msg.sof  = TELEMETRY_MSG_SOF;
    m_msg.size = TELEMETRY_MSG_SIZE_BYTES;
    m_msg.res  = 0;

    if (telemCnt < 4) {
        /* Get attitude data. */
        switch (ui->comboData->currentIndex()) {
        case 0:
            m_msg.msg_id = 'r';
            break;
        case 1:
            m_msg.msg_id = 't';
            break;
        case 2:
            m_msg.msg_id = 'f';
            break;
        case 3:
            m_msg.msg_id = 'g';
            break;
        case 4:
            m_msg.msg_id = 'j';
            break;
        case 5:
            m_msg.msg_id = 'k';
            break;
        case 6:
            m_msg.msg_id = 'l';
            break;
        case 7:
            m_msg.msg_id = 'm';
            break;
        case 8:
            m_msg.msg_id = 'u';
            break;
        case 9:
            m_msg.msg_id = 'v';
            break;
        default:
            m_msg.msg_id = 'r';
        }
        m_msg.crc = GetCRC32Checksum(m_msg);
        SerialDataWrite(m_msg);
        telemCnt++;
    } else {
        /* Get input values. */
        m_msg.msg_id = 'i';
        m_msg.crc    = GetCRC32Checksum(m_msg);
        SerialDataWrite(m_msg);

        /* Get offset values. */
        m_msg.msg_id = 'h';
        m_msg.crc    = GetCRC32Checksum(m_msg);
        SerialDataWrite(m_msg);
        telemCnt = 0;
    }
}


/**
 * @brief MainWindow::HandleDataXClicked
 */
void MainWindow::HandleDataXClicked()
{
    ui->plotData->graph(0)->setVisible(ui->checkDataX->isChecked());
}

/**
 * @brief MainWindow::HandleDataYClicked
 */
void MainWindow::HandleDataYClicked()
{
    ui->plotData->graph(1)->setVisible(ui->checkDataY->isChecked());
}

/**
 * @brief MainWindow::HandleDataZClicked
 */
void MainWindow::HandleDataZClicked()
{
    ui->plotData->graph(2)->setVisible(ui->checkDataZ->isChecked());
}

void MainWindow::HandleAccCalibrate()
{
    m_msg.sof  = TELEMETRY_MSG_SOF;
    m_msg.size = TELEMETRY_MSG_SIZE_BYTES;
    m_msg.res  = 0;

    /* Start accel calibration. */
    m_msg.msg_id = ']';
    m_msg.crc    = GetCRC32Checksum(m_msg);
    SerialDataWrite(m_msg);
}

void MainWindow::HandleGyroCalibrate()
{
    m_msg.sof  = TELEMETRY_MSG_SOF;
    m_msg.size = TELEMETRY_MSG_SIZE_BYTES;
    m_msg.res  = 0;

    /* Start gyro calibration. */
    m_msg.msg_id = '[';
    m_msg.crc    = GetCRC32Checksum(m_msg);
    SerialDataWrite(m_msg);
}

void MainWindow::UpdatePlotData(const float rpy[3])
{
    bool fEnlarge = false;
    double key = QDateTime::currentDateTime().toMSecsSinceEpoch()/1000.0;

    ui->plotData->graph(0)->setVisible(ui->checkDataX->isChecked());
    ui->plotData->graph(1)->setVisible(ui->checkDataY->isChecked());
    ui->plotData->graph(2)->setVisible(ui->checkDataZ->isChecked());
    // add new data:
    ui->plotData->graph(0)->addData(key, rpy[0]);
    ui->plotData->graph(1)->addData(key, rpy[1]);
    ui->plotData->graph(2)->addData(key, rpy[2]);
    // remove data of lines that's outside visible range:
    ui->plotData->graph(0)->removeDataBefore(key-8);
    ui->plotData->graph(1)->removeDataBefore(key-8);
    ui->plotData->graph(2)->removeDataBefore(key-8);
    // rescale value (vertical) axis to fit the current data:
    if (ui->plotData->graph(0)->visible()) {
        ui->plotData->graph(0)->rescaleValueAxis();
        fEnlarge = true;
    }
    if (ui->plotData->graph(1)->visible()) {
        ui->plotData->graph(1)->rescaleValueAxis(fEnlarge);
        fEnlarge = true;
    }
    if (ui->plotData->graph(2)->visible()) {
        ui->plotData->graph(2)->rescaleValueAxis(fEnlarge);
    }

    ui->plotData->xAxis->setRange(key+0.25, 8, Qt::AlignRight);
    ui->plotData->replot();
}

/**
 * @brief MainWindow::ProcessSerialCommands
 * @param pMsg
 */
void MainWindow::ProcessSerialMessages(const TelemetryMessage &msg)
{
    float rpy[3];
    float quat[4];
    fix16_t *pfix16Buf;
    QQuaternion attiQ;
    QQuaternion diffQ;

    switch (msg.msg_id) {
    case 'D': /* Response to new sensor settings. */
        if (strcmp(msg.data, TELEMETRY_RESP_FAIL) == 0) {
            qDebug() << "New sensor settings were not applied!";
        }
        break;
    case 'I': /* Response to new mixed input settings. */
        if (strcmp(msg.data, TELEMETRY_RESP_FAIL) == 0) {
            qDebug() << "New mixed input settings were not applied!";
        }
        break;
    case 'N': /* Response to new input mode settings. */
        if (strcmp(msg.data, TELEMETRY_RESP_FAIL) == 0) {
            qDebug() << "New input mode settings were not applied!";
        }
        break;
    case 'O': /* Response to new output settings. */
        if (strcmp(msg.data, TELEMETRY_RESP_FAIL) == 0) {
            qDebug() << "New output settings were not applied!";
        }
        break;
    case 'S': /* Response to new PID values. */
        if (strcmp(msg.data, TELEMETRY_RESP_FAIL) == 0) {
            qDebug() << "New PID values were not applied!";
        }
        break;
    case 'b': /* Reads board status. */
        if ((msg.size - TELEMETRY_MSG_SIZE_BYTES) == sizeof(boardStatus)) {
            boardStatus = *(msg.data);
            BoardStatusApply();
            qDebug() << "Board status:\r\n  MPU6050 low detected:" << ((boardStatus & 1) == 1) \
                     << "\r\n  MPU6050 high detected:" << ((boardStatus & 2) == 2) \
                     << "\r\n  EEPROM detected:" << ((boardStatus & 4) == 4);
        } else {
            qDebug() << "Board status size mismatch:" << (msg.size - TELEMETRY_MSG_SIZE_BYTES) \
                     << "|" << sizeof(boardStatus);
        }
        break;
    case 'c': /* Response to Save Settings message. */
        if (strcmp(msg.data, TELEMETRY_RESP_FAIL) == 0) {
            qDebug() << "Settings were not saved!";
            QMessageBox::critical(this, tr("Serial Error"), "Save settings failed!");
        } else if (strcmp(msg.data, TELEMETRY_RESP_OK) == 0) {
            qDebug() << "Settings were successfuly saved!";
        }
        break;
    case 'd': /* Reads sensor settings. */
        qDebug() << "Sensor settings received.";
        if ((msg.size - TELEMETRY_MSG_SIZE_BYTES) == sizeof(sensorSettings)) {
            memcpy((void *)sensorSettings, (void *)msg.data, sizeof(sensorSettings));
            SetSensorSettings();
        } else {
            qDebug() << "Sensor settings size mismatch:" << (msg.size - TELEMETRY_MSG_SIZE_BYTES) \
                     << "|" << sizeof(sensorSettings);
        }
        break;
    case 'e': /* Reads I2C error info structure. */
        if ((msg.size - TELEMETRY_MSG_SIZE_BYTES) == sizeof(i2cErrorInfo)) {
            memcpy((void *)&i2cErrorInfo, (void *)msg.data, sizeof(i2cErrorInfo));
            qDebug() << "I2C Error Info:\r\n  I2C last error code:" << i2cErrorInfo.last_i2c_error \
                     << "\r\n  I2C errors:" << i2cErrorInfo.i2c_error_counter;
        } else {
            qDebug() << "I2C error info size mismatch:" << (msg.size - TELEMETRY_MSG_SIZE_BYTES) \
                     << "|" << sizeof(i2cErrorInfo);
        }
        break;
    case 'f': /* Reads IMU1 gyroscope data. */
    case 'g': /* Reads IMU2 gyroscope data. */
        if ((msg.size - TELEMETRY_MSG_SIZE_BYTES) == (sizeof(fix16_t) * 3)) {
            pfix16Buf = (fix16_t *)(msg.data);
            rpy[0] = fix16_to_float(pfix16Buf[0]);
            rpy[1] = fix16_to_float(pfix16Buf[1]);
            rpy[2] = fix16_to_float(pfix16Buf[2]);
            UpdatePlotData(rpy);
        } else {
            qDebug() << "Gyro size mismatch:" << (msg.size - TELEMETRY_MSG_SIZE_BYTES) \
                     << "|" << (sizeof(fix16_t) * 3);
        }
        break;
    case 'h': /* Reads motor offset. */
        if ((msg.size - TELEMETRY_MSG_SIZE_BYTES) == sizeof(motorOffset)) {
            memcpy((void *)motorOffset, (void *)msg.data, sizeof(motorOffset));
            ui->labelOffsetPitch->setText(tr("%1°").arg(round(fix16_to_float(motorOffset[0])*RAD2DEG)));
            ui->labelOffsetRoll->setText(tr("%1°").arg(round(fix16_to_float(motorOffset[1])*RAD2DEG)));
            ui->labelOffsetYaw->setText(tr("%1°").arg(round(fix16_to_float(motorOffset[2])*RAD2DEG)));
        } else {
            qDebug() << "Motor offset size mismatch:" << (msg.size - TELEMETRY_MSG_SIZE_BYTES) \
                     << "|" << (sizeof(float) * 3);
        }
        break;
    case 'i': /* Reads input values. */
        if ((msg.size - TELEMETRY_MSG_SIZE_BYTES) == sizeof(inputValues)) {
            memcpy((void *)inputValues, (void *)msg.data, sizeof(inputValues));
            if (inSettings[0].channel_id < 5) {
                ui->labelInputPitch->setText(tr("%1").arg(inputValues[inSettings[0].channel_id]));
            } else {
                ui->labelInputPitch->setText(tr("%1").arg(inSettings[0].min_val));
            }
            if (inSettings[1].channel_id < 5) {
                ui->labelInputRoll->setText(tr("%1").arg(inputValues[inSettings[1].channel_id]));
            } else {
                ui->labelInputRoll->setText(tr("%1").arg(inSettings[1].min_val));
            }
            if (inSettings[2].channel_id < 5) {
                ui->labelInputYaw->setText(tr("%1").arg(inputValues[inSettings[2].channel_id]));
            } else {
                ui->labelInputYaw->setText(tr("%1").arg(inSettings[2].min_val));
            }
        } else {
            qDebug() << "Input values size mismatch:" << (msg.size - TELEMETRY_MSG_SIZE_BYTES) \
                     << "|" << sizeof(inputValues);
        }
        break;
    case 'j': /* Reads IMU1 accelerometer data. */
    case 'k': /* Reads IMU2 accelerometer data. */
        if ((msg.size - TELEMETRY_MSG_SIZE_BYTES) == (sizeof(fix16_t) * 3)) {
            pfix16Buf = (fix16_t *)(msg.data);
            rpy[0] = fix16_to_float(pfix16Buf[0]);
            rpy[1] = fix16_to_float(pfix16Buf[1]);
            rpy[2] = fix16_to_float(pfix16Buf[2]);
            UpdatePlotData(rpy);
        } else {
            qDebug() << "Acc size mismatch:" << (msg.size - TELEMETRY_MSG_SIZE_BYTES) \
                     << "|" << (sizeof(fix16_t) * 3);
        }
        break;
    case 'l': /* Reads IMU1 gyroscope bias data. */
    case 'm': /* Reads IMU2 gyroscope bias data. */
        if ((msg.size - TELEMETRY_MSG_SIZE_BYTES) == (sizeof(fix16_t) * 3)) {
            pfix16Buf = (fix16_t *)(msg.data);
            rpy[0] = fix16_to_float(pfix16Buf[0]);
            rpy[1] = fix16_to_float(pfix16Buf[1]);
            rpy[2] = fix16_to_float(pfix16Buf[2]);
            UpdatePlotData(rpy);
        } else {
            qDebug() << "Gyro bias size mismatch:" << (msg.size - TELEMETRY_MSG_SIZE_BYTES) \
                     << "|" << (sizeof(fix16_t) * 3);
        }
        break;
    case 'n': /* Reads input mode settings. */
        qDebug() << "Input mode settings received.";
        if ((msg.size - TELEMETRY_MSG_SIZE_BYTES) == sizeof(modeSettings)) {
            memcpy((void *)modeSettings, (void *)msg.data, sizeof(modeSettings));
            SetInputModeSettings();
        } else {
            qDebug() << "Input mode settings size mismatch:" << (msg.size - TELEMETRY_MSG_SIZE_BYTES) \
                     << "|" << sizeof(modeSettings);
        }
        break;
    case 'o': /* Reads output settings. */
        qDebug() << "Output settings received.";
        if ((msg.size - TELEMETRY_MSG_SIZE_BYTES) == sizeof(outSettings)) {
            memcpy((void *)outSettings, (void *)msg.data, sizeof(outSettings));
            SetOutputSettings();
        } else {
            qDebug() << "Output settings size mismatch:" << (msg.size - TELEMETRY_MSG_SIZE_BYTES) \
                     << "|" << sizeof(outSettings);
        }
        break;
    case 'p': /* Reads input settings. */
        qDebug() << "Input settings received.";
        if ((msg.size - TELEMETRY_MSG_SIZE_BYTES) == sizeof(inSettings)) {
            memcpy((void *)inSettings, (void *)msg.data, sizeof(inSettings));
            SetInputSettings();
        } else {
            qDebug() << "Input settings size mismatch:" << (msg.size - TELEMETRY_MSG_SIZE_BYTES) \
                     << "|" << sizeof(inSettings);
        }
        break;
    case 'r': /* Reads camera attitude quaternion. */
    case 't': /* Reads gimbal frame attitude quaternion. */
        if ((msg.size - TELEMETRY_MSG_SIZE_BYTES) == (sizeof(fix16_t) * 4)) {
            pfix16Buf = (fix16_t *)(msg.data);
            quat[0] = fix16_to_float(pfix16Buf[0]);
            quat[1] = fix16_to_float(pfix16Buf[1]);
            quat[2] = fix16_to_float(pfix16Buf[2]);
            quat[3] = fix16_to_float(pfix16Buf[3]);

            Quaternion2RPY(quat, rpy);
            rpy[0] *= RAD2DEG;
            rpy[1] *= RAD2DEG;
            rpy[2] *= RAD2DEG;
            UpdatePlotData(rpy);

            /*
             * Construct an attitude quaternion.
             * WARNING! OpenGL and IMU coordinate systems do not match.
             * Adjust indexes and direction of rotations of IMU data to match
             * OpenGL coordinate system.
             */
            attiQ = QQuaternion(quat[0], quat[1], -quat[3], -quat[2]);

            /*
             * Find the difference between last attitude and current attitude.
             * diffQ is a rotation quaternion that transforms lastQ into attiQ.
             * Because attiQ is normalized quaternion, the inverse of attiQ is equal to
             * conjugate of attiQ.
             * NOTE! The order of multiplication IS IMPORTANT!
            */
            diffQ = attiQ.conjugate() * lastQ;
            diffQ.normalize();

            lastQ = attiQ;

            ui->widget->rotateBy(&diffQ);
        } else {
            qDebug() << "RPY size mismatch:" << (msg.size - TELEMETRY_MSG_SIZE_BYTES) \
                     << "|" << (sizeof(float) * 4);
        }
        break;
    case 's': /* Reads PID settings. */
        qDebug() << "PID settings received.";
        if ((msg.size - TELEMETRY_MSG_SIZE_BYTES) == sizeof(PID)) {
            memcpy((void *)PID, (void *)msg.data, sizeof(PID));
            SetStabilizationSettings();
        } else {
            qDebug() << "PID settings CRC32 mismatch:" << (msg.crc - TELEMETRY_MSG_SIZE_BYTES) \
                     << "|" << sizeof(PID);
        }
        break;
    case 'u': /* Reads IMU1 accelerometer error. */
    case 'v': /* Reads IMU2 accelerometer error. */
        if ((msg.size - TELEMETRY_MSG_SIZE_BYTES) == (sizeof(fix16_t) * 3)) {
            pfix16Buf = (fix16_t *)(msg.data);
            rpy[0] = fix16_to_float(pfix16Buf[0]);
            rpy[1] = fix16_to_float(pfix16Buf[1]);
            rpy[2] = fix16_to_float(pfix16Buf[2]);
            UpdatePlotData(rpy);
        } else {
            qDebug() << "Acc error size mismatch:" << (msg.size - TELEMETRY_MSG_SIZE_BYTES) \
                     << "|" << (sizeof(fix16_t) * 3);
        }
        break;
    default:
        qDebug() << "Unhandled message received!";
    }
}

void MainWindow::BoardStatusApply()
{
    /* Check if sensor1 detected. */
    if (boardStatus & MPU6050_LOW_DETECTED) {
        ui->groupSensor1->setTitle("Sensor1 (MPU6050):");
        ui->comboSensor1AxisTOP->setEnabled(true);
        ui->comboSensor1AxisRIGHT->setEnabled(true);
        ui->pushSensor1AccCalibrate->setEnabled(true);
        ui->pushSensor1GyroCalibrate->setEnabled(true);
    } else {
        ui->groupSensor1->setTitle("Sensor1 (none):");
        ui->comboSensor1AxisTOP->setEnabled(false);
        ui->comboSensor1AxisRIGHT->setEnabled(false);
        ui->pushSensor1AccCalibrate->setEnabled(false);
        ui->pushSensor1GyroCalibrate->setEnabled(false);
    }
    /* Check if sensor1 detected. */
    if (boardStatus & MPU6050_HIGH_DETECTED) {
        ui->groupSensor2->setTitle("Sensor2 (MPU6050):");
        ui->comboSensor2AxisTOP->setEnabled(true);
        ui->comboSensor2AxisRIGHT->setEnabled(true);
        ui->pushSensor2AccCalibrate->setEnabled(true);
        ui->pushSensor2GyroCalibrate->setEnabled(true);
    } else {
        ui->groupSensor2->setTitle("Sensor2 (none):");
        ui->comboSensor2AxisTOP->setEnabled(false);
        ui->comboSensor2AxisRIGHT->setEnabled(false);
        ui->pushSensor2AccCalibrate->setEnabled(false);
        ui->pushSensor2GyroCalibrate->setEnabled(false);
    }
    /* Check if EEPROM detected. */
    if (boardStatus & EEPROM_24C02_DETECTED) {
        ui->actionSave->setEnabled(true);
    } else {
        ui->actionSave->setEnabled(false);
    }
}

/**
 * @brief MainWindow::GetStabilizationSettings
 * @return
 */
bool MainWindow::GetStabilizationSettings()
{
    bool fUpdated = false;
    quint8 temp;

    /* PITCH */
    temp = ui->spinPitch_P->value();
    if (temp != PID[0].P) {
        PID[0].P = temp;
        fUpdated = true;
    }
    temp = ui->spinPitch_I->value();
    if (temp != PID[0].I) {
        PID[0].I = temp;
        fUpdated = true;
    }
    temp = ui->spinPitch_D->value();
    if (temp != PID[0].D) {
        PID[0].D = temp;
        fUpdated = true;
    }
    temp = ui->spinPitch_F->value();
    if (temp != PID[0].F) {
        PID[0].F = temp;
        fUpdated = true;
    }

    /* ROLL */
    temp = ui->spinRoll_P->value();
    if (temp != PID[1].P) {
        PID[1].P = temp;
        fUpdated = true;
    }
    temp = ui->spinRoll_I->value();
    if (temp != PID[1].I) {
        PID[1].I = temp;
        fUpdated = true;
    }
    temp = ui->spinRoll_D->value();
    if (temp != PID[1].D) {
        PID[1].D = temp;
        fUpdated = true;
    }
    temp = ui->spinRoll_F->value();
    if (temp != PID[1].F) {
        PID[1].F = temp;
        fUpdated = true;
    }

    /* YAW */
    temp = ui->spinYaw_P->value();
    if (temp != PID[2].P) {
        PID[2].P = temp;
        fUpdated = true;
    }
    temp = ui->spinYaw_I->value();
    if (temp != PID[2].I) {
        PID[2].I = temp;
        fUpdated = true;
    }
    temp = ui->spinYaw_D->value();
    if (temp != PID[2].D) {
        PID[2].D = temp;
        fUpdated = true;
    }
    temp = ui->spinYaw_F->value();
    if (temp != PID[2].F) {
        PID[2].F = temp;
        fUpdated = true;
    }

    return fUpdated;
}

/**
 * @brief MainWindow::SetStabilizationSettings
 * @return
 */
bool MainWindow::SetStabilizationSettings()
{
    /* PITCH */
    ui->spinPitch_P->setValue(PID[0].P);
    ui->spinPitch_I->setValue(PID[0].I);
    ui->spinPitch_D->setValue(PID[0].D);
    ui->spinPitch_F->setValue(PID[0].F);
    /* ROLL */
    ui->spinRoll_P->setValue(PID[1].P);
    ui->spinRoll_I->setValue(PID[1].I);
    ui->spinRoll_D->setValue(PID[1].D);
    ui->spinRoll_F->setValue(PID[1].F);
    /* YAW */
    ui->spinYaw_P->setValue(PID[2].P);
    ui->spinYaw_I->setValue(PID[2].I);
    ui->spinYaw_D->setValue(PID[2].D);
    ui->spinYaw_F->setValue(PID[2].F);
    return true;
}

/**
 * @brief MainWindow::GetOutputSettings
 * @return
 */
bool MainWindow::GetOutputSettings()
{
    bool fUpdated = false;
    quint8 temp;
    quint8 temp2 = 0;

    /* PITCH */
    temp = ui->spinPitchPower->value();
    if (temp != outSettings[0].power) {
        outSettings[0].power = temp;
        fUpdated = true;
    }
    temp = ui->spinPitchNumPoles->value();
    if (temp != outSettings[0].num_poles) {
        outSettings[0].num_poles = temp;
        fUpdated = true;
    }
    temp = ui->checkPitchRev->isChecked();
    if (temp != (outSettings[0].flags & PWM_OUT_REV_FLAG)) {
        fUpdated = true;
    }
    temp2 = temp;
    temp = ui->checkPitchTHIEnable->isChecked() << 1;
    if (temp != (outSettings[0].flags & PWM_OUT_THI_FLAG)) {
        fUpdated = true;
    }
    outSettings[0].flags = temp | temp2;
    temp2 = 0;
    temp = ui->comboPitchCommand->currentIndex();
    if (temp != (outSettings[0].dt_cmd_id & PWM_OUT_CMD_ID_MASK)) {
        fUpdated = true;
    }
    temp2 = temp;
    temp = ui->comboPitchDeadTime->currentIndex() << 4;
    if (temp != (outSettings[0].dt_cmd_id & PWM_OUT_DT_ID_MASK)) {
        fUpdated = true;
    }
    outSettings[0].dt_cmd_id = temp | temp2;
    temp2 = 0;

    /* ROLL */
    temp = ui->spinRollPower->value();
    if (temp != outSettings[1].power) {
        outSettings[1].power = temp;
        fUpdated = true;
    }
    temp = ui->spinRollNumPoles->value();
    if (temp != outSettings[1].num_poles) {
        outSettings[1].num_poles = temp;
        fUpdated = true;
    }
    temp = ui->checkRollRev->isChecked();
    if (temp != (outSettings[1].flags & PWM_OUT_REV_FLAG)) {
        fUpdated = true;
    }
    temp2 = temp;
    temp = ui->checkRollTHIEnable->isChecked() << 1;
    if (temp != (outSettings[1].flags & PWM_OUT_THI_FLAG)) {
        fUpdated = true;
    }
    outSettings[1].flags = temp | temp2;
    temp2 = 0;
    temp = ui->comboRollCommand->currentIndex();
    if (temp != (outSettings[1].dt_cmd_id & PWM_OUT_CMD_ID_MASK)) {
        fUpdated = true;
    }
    temp2 = temp;
    temp = ui->comboRollDeadTime->currentIndex() << 4;
    if (temp != (outSettings[1].dt_cmd_id & PWM_OUT_DT_ID_MASK)) {
        fUpdated = true;
    }
    outSettings[1].dt_cmd_id = temp | temp2;
    temp2 = 0;

    /* YAW */
    temp = ui->spinYawPower->value();
    if (temp != outSettings[2].power) {
        outSettings[2].power = temp;
        fUpdated = true;
    }
    temp = ui->spinYawNumPoles->value();
    if (temp != outSettings[2].num_poles) {
        outSettings[2].num_poles = temp;
        fUpdated = true;
    }
    temp = ui->checkYawRev->isChecked();
    if (temp != (outSettings[2].flags & PWM_OUT_REV_FLAG)) {
        fUpdated = true;
    }
    temp2 = temp;
    temp = ui->checkYawTHIEnable->isChecked() << 1;
    if (temp != (outSettings[2].flags & PWM_OUT_THI_FLAG)) {
        fUpdated = true;
    }
    outSettings[2].flags = temp | temp2;
    temp2 = 0;
    temp = ui->comboYawCommand->currentIndex();
    if (temp != (outSettings[2].dt_cmd_id & PWM_OUT_CMD_ID_MASK)) {
        fUpdated = true;
    }
    temp2 = temp;
    temp = ui->comboYawDeadTime->currentIndex() << 4;
    if (temp != (outSettings[2].dt_cmd_id & PWM_OUT_DT_ID_MASK)) {
        fUpdated = true;
    }
    outSettings[2].dt_cmd_id = temp | temp2;
    return fUpdated;
}

/**
 * @brief MainWindow::SetOutputSettings
 * @return
 */
bool MainWindow::SetOutputSettings()
{
    /* PITCH */
    ui->spinPitchPower->setValue(outSettings[0].power);
    ui->spinPitchNumPoles->setValue(outSettings[0].num_poles);
    ui->checkPitchRev->setChecked(outSettings[0].flags & PWM_OUT_REV_FLAG);
    ui->checkPitchTHIEnable->setChecked(outSettings[0].flags & PWM_OUT_THI_FLAG);
    ui->comboPitchCommand->setCurrentIndex(outSettings[0].dt_cmd_id & 0x0F);
    ui->comboPitchDeadTime->setCurrentIndex(outSettings[0].dt_cmd_id >> 4);
    /* ROLL */
    ui->spinRollPower->setValue(outSettings[1].power);
    ui->spinRollNumPoles->setValue(outSettings[1].num_poles);
    ui->checkRollRev->setChecked(outSettings[1].flags & PWM_OUT_REV_FLAG);
    ui->checkRollTHIEnable->setChecked(outSettings[1].flags & PWM_OUT_THI_FLAG);
    ui->comboRollCommand->setCurrentIndex(outSettings[1].dt_cmd_id & 0x0F);
    ui->comboRollDeadTime->setCurrentIndex(outSettings[1].dt_cmd_id >> 4);
    /* YAW */
    ui->spinYawPower->setValue(outSettings[2].power);
    ui->spinYawNumPoles->setValue(outSettings[2].num_poles);
    ui->checkYawRev->setChecked(outSettings[2].flags & PWM_OUT_REV_FLAG);
    ui->checkYawTHIEnable->setChecked(outSettings[2].flags & PWM_OUT_THI_FLAG);
    ui->comboYawCommand->setCurrentIndex(outSettings[2].dt_cmd_id & 0x0F);
    ui->comboYawDeadTime->setCurrentIndex(outSettings[2].dt_cmd_id >> 4);
    return true;
}

/**
 * @brief MainWindow::GetInputSettings
 * @return
 */
bool MainWindow::GetInputSettings()
{
    bool fUpdated = false;
    quint16 temp;

    /* PITCH */
    temp = ui->comboInputChannelPitch->currentIndex();
    if (temp != inSettings[0].channel_id) {
        inSettings[0].channel_id = (quint8)temp;
        fUpdated = true;
    }
    temp = ui->spinInputMinPitch->value();
    if (temp != inSettings[0].min_val) {
        inSettings[0].min_val = temp;
        fUpdated = true;
    }
    temp = ui->spinInputMidPitch->value();
    if (temp != inSettings[0].mid_val) {
        inSettings[0].mid_val = temp;
        fUpdated = true;
    }
    temp = ui->spinInputMaxPitch->value();
    if (temp != inSettings[0].max_val) {
        inSettings[0].max_val = temp;
        fUpdated = true;
    }

    /* ROLL */
    temp = ui->comboInputChannelRoll->currentIndex();
    if (temp != inSettings[1].channel_id) {
        inSettings[1].channel_id = (quint8)temp;
        fUpdated = true;
    }
    temp = ui->spinInputMinRoll->value();
    if (temp != inSettings[1].min_val) {
        inSettings[1].min_val = temp;
        fUpdated = true;
    }
    temp = ui->spinInputMidRoll->value();
    if (temp != inSettings[1].mid_val) {
        inSettings[1].mid_val = temp;
        fUpdated = true;
    }
    temp = ui->spinInputMaxRoll->value();
    if (temp != inSettings[1].max_val) {
        inSettings[1].max_val = temp;
        fUpdated = true;
    }

    /* YAW */
    temp = ui->comboInputChannelYaw->currentIndex();
    if (temp != inSettings[2].channel_id) {
        inSettings[2].channel_id = (quint8)temp;
        fUpdated = true;
    }
    temp = ui->spinInputMinYaw->value();
    if (temp != inSettings[2].min_val) {
        inSettings[2].min_val = temp;
        fUpdated = true;
    }
    temp = ui->spinInputMidYaw->value();
    if (temp != inSettings[2].mid_val) {
        inSettings[2].mid_val = temp;
        fUpdated = true;
    }
    temp = ui->spinInputMaxYaw->value();
    if (temp != inSettings[2].max_val) {
        inSettings[2].max_val = temp;
        fUpdated = true;
    }
    return fUpdated;
}

/**
 * @brief MainWindow::SetInputSettings
 * @return
 */
bool MainWindow::SetInputSettings()
{
    /* PITCH */
    ui->comboInputChannelPitch->setCurrentIndex(inSettings[0].channel_id);
    ui->spinInputMinPitch->setValue(inSettings[0].min_val);
    ui->spinInputMidPitch->setValue(inSettings[0].mid_val);
    ui->spinInputMaxPitch->setValue(inSettings[0].max_val);
    /* ROLL */
    ui->comboInputChannelRoll->setCurrentIndex(inSettings[1].channel_id);
    ui->spinInputMinRoll->setValue(inSettings[1].min_val);
    ui->spinInputMidRoll->setValue(inSettings[1].mid_val);
    ui->spinInputMaxRoll->setValue(inSettings[1].max_val);
    /* YAW */
    ui->comboInputChannelYaw->setCurrentIndex(inSettings[2].channel_id);
    ui->spinInputMinYaw->setValue(inSettings[2].min_val);
    ui->spinInputMidYaw->setValue(inSettings[2].mid_val);
    ui->spinInputMaxYaw->setValue(inSettings[2].max_val);
    return true;
}

/**
 * @brief MainWindow::GetInputModeSettings
 * @return
 */
bool MainWindow::GetInputModeSettings()
{
    bool fUpdated = false;
    qint16 temp;

    /* PITCH */
    temp = ui->comboInputModePitch->currentIndex();
    if (temp != modeSettings[0].mode_id) {
        modeSettings[0].mode_id = (quint8)temp;
        fUpdated = true;
    }
    temp = ui->spinInputMinAnglePitch->value();
    if (temp != modeSettings[0].min_angle) {
        modeSettings[0].min_angle = temp;
        fUpdated = true;
    }
    temp = ui->spinInputMaxAnglePitch->value();
    if (temp != modeSettings[0].max_angle) {
        modeSettings[0].max_angle = temp;
        fUpdated = true;
    }
    temp = ui->spinInputSpeedPitch->value();
    if (temp != modeSettings[0].speed) {
        modeSettings[0].speed = temp;
        fUpdated = true;
    }
    temp = ui->spinInputOffsetPitch->value();
    if (temp != modeSettings[0].offset) {
        modeSettings[0].offset = temp;
        fUpdated = true;
    }

    /* ROLL */
    temp = ui->comboInputModeRoll->currentIndex();
    if (temp != modeSettings[1].mode_id) {
        modeSettings[1].mode_id = (quint8)temp;
        fUpdated = true;
    }
    temp = ui->spinInputMinAngleRoll->value();
    if (temp != modeSettings[1].min_angle) {
        modeSettings[1].min_angle = temp;
        fUpdated = true;
    }
    temp = ui->spinInputMaxAngleRoll->value();
    if (temp != modeSettings[1].max_angle) {
        modeSettings[1].max_angle = temp;
        fUpdated = true;
    }
    temp = ui->spinInputSpeedRoll->value();
    if (temp != modeSettings[1].speed) {
        modeSettings[1].speed = temp;
        fUpdated = true;
    }
    temp = ui->spinInputOffsetRoll->value();
    if (temp != modeSettings[1].offset) {
        modeSettings[1].offset = temp;
        fUpdated = true;
    }

    /* YAW */
    temp = ui->comboInputModeYaw->currentIndex();
    if (temp != modeSettings[2].mode_id) {
        modeSettings[2].mode_id = (quint8)temp;
        fUpdated = true;
    }
    temp = ui->spinInputMinAngleYaw->value();
    if (temp != modeSettings[2].min_angle) {
        modeSettings[2].min_angle = temp;
        fUpdated = true;
    }
    temp = ui->spinInputMaxAngleYaw->value();
    if (temp != modeSettings[2].max_angle) {
        modeSettings[2].max_angle = temp;
        fUpdated = true;
    }
    temp = ui->spinInputSpeedYaw->value();
    if (temp != modeSettings[2].speed) {
        modeSettings[2].speed = temp;
        fUpdated = true;
    }
    temp = ui->spinInputOffsetYaw->value();
    if (temp != modeSettings[2].offset) {
        modeSettings[2].offset = temp;
        fUpdated = true;
    }
    return fUpdated;
}

/**
 * @brief MainWindow::SetInputModeSettings
 * @return
 */
bool MainWindow::SetInputModeSettings()
{
    /* PITCH */
    ui->comboInputModePitch->setCurrentIndex(modeSettings[0].mode_id);
    ui->spinInputMinAnglePitch->setValue(modeSettings[0].min_angle);
    ui->spinInputMaxAnglePitch->setValue(modeSettings[0].max_angle);
    ui->spinInputSpeedPitch->setValue(modeSettings[0].speed);
    ui->spinInputOffsetPitch->setValue(modeSettings[0].offset);
    /* ROLL */
    ui->comboInputModeRoll->setCurrentIndex(modeSettings[1].mode_id);
    ui->spinInputMinAngleRoll->setValue(modeSettings[1].min_angle);
    ui->spinInputMaxAngleRoll->setValue(modeSettings[1].max_angle);
    ui->spinInputSpeedRoll->setValue(modeSettings[1].speed);
    ui->spinInputOffsetRoll->setValue(modeSettings[1].offset);
    /* YAW */
    ui->comboInputModeYaw->setCurrentIndex(modeSettings[2].mode_id);
    ui->spinInputMinAngleYaw->setValue(modeSettings[2].min_angle);
    ui->spinInputMaxAngleYaw->setValue(modeSettings[2].max_angle);
    ui->spinInputSpeedYaw->setValue(modeSettings[2].speed);
    ui->spinInputOffsetYaw->setValue(modeSettings[2].offset);
    return true;
}

/**
 * @brief MainWindow::GetSensorSettings
 * @return
 */
bool MainWindow::GetSensorSettings()
{
    bool fUpdated = false;
    quint8 temp;
    quint8 temp2;

    /* TOP axis */
    temp = ui->comboSensor1AxisTOP->currentIndex();
    if (temp != (sensorAxes[2] & 0x0F)) {
        fUpdated = true;
    }
    temp2 = temp;
    temp = ui->comboSensor2AxisTOP->currentIndex() << 4;
    if (temp != (sensorAxes[2] & 0xF0)) {
        fUpdated = true;
    }
    sensorAxes[2] = temp | temp2;

    /* RIGHT axis */
    temp = ui->comboSensor1AxisRIGHT->currentIndex();
    if (temp != (sensorAxes[0] & 0x0F)) {
        fUpdated = true;
    }
    temp2 = temp;
    temp = ui->comboSensor2AxisRIGHT->currentIndex() << 4;
    if (temp != (sensorAxes[0] & 0xF0)) {
        fUpdated = true;
    }
    sensorAxes[0] = temp | temp2;

    /* FRONT axis */
    if (fUpdated) {
        temp2 = amMtx[(sensorAxes[0] & 0x0F)][(sensorAxes[2] & 0x0F)];
        temp  = amMtx[(sensorAxes[0] >> 4)][(sensorAxes[2] >> 4)] << 4;
        if ((temp2 == 0x06) || (temp == 0x60)) {
            QMessageBox::critical(this, tr("Axes error"),
                "Top and right axes of the sensor cannot be the same!");
            return false;
        }
        sensorAxes[1] = temp | temp2;

        for (quint8 i = 0; i < 3; i++) {
            sensorSettings[i] = 0;
            /* Sensor 1: */
            switch (sensorAxes[i] & 0x0F) {
            case 0x00: // +X;
                // Axis ID = 0;
                // Axis dir = negative;
                break;
            case 0x01: // +Y;
                sensorSettings[i] |= 0x01;
                sensorSettings[i] |= SENSOR1_AXIS_DIR_POS;
                break;
            case 0x02: // +Z;
                sensorSettings[i] |= 0x02;
                // Axis dir = negative;
                break;
            case 0x03: // -X;
                // Axis ID = 0;
                sensorSettings[i] |= SENSOR1_AXIS_DIR_POS;
                break;
            case 0x04: // -Y;
                sensorSettings[i] |= 0x01;
                // Axis dir = negative;
                break;
            case 0x05: // -Z;
                sensorSettings[i] |= 0x02;
                sensorSettings[i] |= SENSOR1_AXIS_DIR_POS;
                break;
            }
            /* Sensor 2: */
            switch (sensorAxes[i] & 0xF0) {
            case 0x00: // +X;
                // Axis ID = 0;
                // Axis dir = negative;
                break;
            case 0x10: // +Y;
                sensorSettings[i] |= 0x10;
                sensorSettings[i] |= SENSOR2_AXIS_DIR_POS;
                break;
            case 0x20: // +Z;
                sensorSettings[i] |= 0x20;
                // Axis dir = negative;
                break;
            case 0x30: // -X;
                // Axis ID = 0;
                sensorSettings[i] |= SENSOR2_AXIS_DIR_POS;
                break;
            case 0x40: // -Y;
                sensorSettings[i] |= 0x10;
                // Axis dir = negative;
                break;
            case 0x50: // -Z;
                sensorSettings[i] |= 0x20;
                sensorSettings[i] |= SENSOR2_AXIS_DIR_POS;
                break;
            }
        }
    }

    return fUpdated;
}

/**
 * @brief MainWindow::SetSensorSettings
 * @return
 */
bool MainWindow::SetSensorSettings()
{
    for (quint8 i = 0; i < 3; i++) {
        sensorAxes[i] = 0x00;
        /* Sensor 1: */
        switch (sensorSettings[i] & SENSOR1_AXIS_ID_MASK) {
        case 0x00: // X;
            if (sensorSettings[i] & SENSOR1_AXIS_DIR_POS) {
                sensorAxes[i] |= 0x03;
            } else {
                sensorAxes[i] |= 0x00;
            }
            break;
        case 0x01: // Y;
            if (sensorSettings[i] & SENSOR1_AXIS_DIR_POS) {
                sensorAxes[i] |= 0x01;
            } else {
                sensorAxes[i] |= 0x04;
            }
            break;
        case 0x02: // Z;
            if (sensorSettings[i] & SENSOR1_AXIS_DIR_POS) {
                sensorAxes[i] |= 0x05;
            } else {
                sensorAxes[i] |= 0x02;
            }
            break;
        }
        /* Sensor 2: */
        switch (sensorSettings[i] & SENSOR2_AXIS_ID_MASK) {
        case 0x00: // X;
            if (sensorSettings[i] & SENSOR2_AXIS_DIR_POS) {
                sensorAxes[i] |= 0x30;
            } else {
                sensorAxes[i] |= 0x00;
            }
            break;
        case 0x10: // Y;
            if (sensorSettings[i] & SENSOR2_AXIS_DIR_POS) {
                sensorAxes[i] |= 0x10;
            } else {
                sensorAxes[i] |= 0x40;
            }
            break;
        case 0x20: // Z;
            if (sensorSettings[i] & SENSOR2_AXIS_DIR_POS) {
                sensorAxes[i] |= 0x50;
            } else {
                sensorAxes[i] |= 0x20;
            }
            break;
        }
    }
    /* Sensor 1: */
    ui->comboSensor1AxisTOP->setCurrentIndex(sensorAxes[2] & 0x0F);
    ui->comboSensor1AxisRIGHT->setCurrentIndex(sensorAxes[0] & 0x0F);
    /* Sensor 2: */
    ui->comboSensor2AxisTOP->setCurrentIndex(sensorAxes[2] >> 4);
    ui->comboSensor2AxisRIGHT->setCurrentIndex(sensorAxes[0] >> 4);
    return true;
}
