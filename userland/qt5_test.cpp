/*
 * AscentOS Qt5 System Dashboard
 * A test Qt5 application for AscentOS demonstrating:
 *   - QMainWindow with menus and status bar
 *   - Animated custom widget (sine wave)
 *   - Live uptime label updated by a QTimer
 *   - QPushButton interactions
 *   - QTabWidget with multiple pages
 */

#include <QApplication>
#include <QMainWindow>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QTabWidget>
#include <QPainter>
#include <QPainterPath>
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QStatusBar>
#include <QProgressBar>
#include <QSlider>
#include <QGroupBox>
#include <QFont>
#include <QColor>
#include <QPalette>
#include <QMessageBox>
#include <QString>
#include <cmath>
#include <cstdio>
#include <sys/utsname.h>
#include <unistd.h>

// ── Custom uptime syscall (AscentOS syscall 399) ────────────────────────────
static long get_uptime_ms() {
    long ms = 0;
    __asm__ volatile(
        "syscall"
        : "=a"(ms)
        : "a"(399)
        : "rcx", "r11", "memory"
    );
    return ms;
}

// ── Animated wave widget ────────────────────────────────────────────────────
class WaveWidget : public QWidget {
public:
    explicit WaveWidget(QWidget *parent = nullptr) : QWidget(parent), m_phase(0.0) {
        setMinimumSize(400, 120);
    }

    void advance(double delta) {
        m_phase += delta;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        // Background gradient
        QLinearGradient bg(0, 0, 0, height());
        bg.setColorAt(0.0, QColor(20,  24,  40));
        bg.setColorAt(1.0, QColor(10,  14,  28));
        p.fillRect(rect(), bg);

        // Grid lines
        p.setPen(QPen(QColor(50, 60, 90), 1, Qt::DotLine));
        for (int y = height() / 4; y < height(); y += height() / 4)
            p.drawLine(0, y, width(), y);

        // Wave 1 — blue
        QPainterPath path1;
        bool first = true;
        for (int x = 0; x < width(); ++x) {
            double t  = static_cast<double>(x) / width();
            double y1 = height() / 2.0
                        - (height() * 0.35) * std::sin(t * 2 * M_PI * 3 + m_phase);
            if (first) { path1.moveTo(x, y1); first = false; }
            else        path1.lineTo(x, y1);
        }
        p.setPen(QPen(QColor(80, 160, 255), 2));
        p.drawPath(path1);

        // Wave 2 — purple (phase-shifted)
        QPainterPath path2;
        first = true;
        for (int x = 0; x < width(); ++x) {
            double t  = static_cast<double>(x) / width();
            double y2 = height() / 2.0
                        - (height() * 0.22) * std::sin(t * 2 * M_PI * 5 + m_phase * 1.3 + 1.0);
            if (first) { path2.moveTo(x, y2); first = false; }
            else        path2.lineTo(x, y2);
        }
        p.setPen(QPen(QColor(200, 100, 255), 2));
        p.drawPath(path2);

        // Label overlay
        p.setPen(QColor(180, 200, 255));
        QFont lf("Monospace", 8);
        p.setFont(lf);
        p.drawText(8, height() - 8, "AscentOS · Qt5 Waveform Display");
    }

private:
    double m_phase;
};

// ── System info tab ─────────────────────────────────────────────────────────
static QWidget *makeInfoTab() {
    QWidget *w = new QWidget;
    QVBoxLayout *vl = new QVBoxLayout(w);
    vl->setSpacing(8);
    vl->setContentsMargins(16, 16, 16, 16);

    auto addRow = [&](const QString &title, const QString &value) {
        QHBoxLayout *hl = new QHBoxLayout;
        QLabel *tl = new QLabel("<b>" + title + "</b>");
        tl->setFixedWidth(130);
        QLabel *vl2 = new QLabel(value);
        vl2->setStyleSheet("color:#8bc8ff;");
        hl->addWidget(tl);
        hl->addWidget(vl2, 1);
        vl->addLayout(hl);
    };

    struct utsname u;
    uname(&u);
    addRow("OS:",         "AscentOS x86_64");
    addRow("Kernel:",     QString(u.release));
    addRow("Machine:",    QString(u.machine));
    addRow("Qt version:", QString(QT_VERSION_STR));
    addRow("Compiler:",   "x86_64-linux-musl-g++");

    // Uptime row (updated externally via label pointer stored in property)
    QHBoxLayout *uhl = new QHBoxLayout;
    QLabel *utl = new QLabel("<b>Uptime:</b>");
    utl->setFixedWidth(130);
    QLabel *uvl = new QLabel("--:--:--");
    uvl->setStyleSheet("color:#8bc8ff;");
    uvl->setObjectName("uptimeLabel");
    uhl->addWidget(utl);
    uhl->addWidget(uvl, 1);
    vl->addLayout(uhl);

    vl->addStretch();
    return w;
}

// ── Controls tab ────────────────────────────────────────────────────────────
static QWidget *makeControlsTab(QProgressBar **outProgress) {
    QWidget *w = new QWidget;
    QVBoxLayout *vl = new QVBoxLayout(w);
    vl->setSpacing(12);
    vl->setContentsMargins(16, 16, 16, 16);

    QGroupBox *grp = new QGroupBox("Progress demo");
    QVBoxLayout *gl = new QVBoxLayout(grp);

    QProgressBar *pb = new QProgressBar;
    pb->setRange(0, 100);
    pb->setValue(0);
    pb->setStyleSheet(
        "QProgressBar { border:1px solid #334; border-radius:4px; background:#1a1e30; height:18px; }"
        "QProgressBar::chunk { background: qlineargradient(x1:0,y1:0,x2:1,y2:0,"
        "stop:0 #3070d0, stop:1 #a040e0); border-radius:3px; }");
    gl->addWidget(pb);
    *outProgress = pb;

    QSlider *sl = new QSlider(Qt::Horizontal);
    sl->setRange(0, 100);
    QObject::connect(sl, &QSlider::valueChanged, pb, &QProgressBar::setValue);
    gl->addWidget(new QLabel("Drag to set progress:"));
    gl->addWidget(sl);

    vl->addWidget(grp);

    QGroupBox *btnGrp = new QGroupBox("Actions");
    QHBoxLayout *bl = new QHBoxLayout(btnGrp);

    auto makeBtn = [](const QString &text, const QString &color) -> QPushButton* {
        QPushButton *b = new QPushButton(text);
        b->setStyleSheet(QString(
            "QPushButton { background:%1; color:#fff; border:none; border-radius:5px;"
            "  padding:6px 14px; font-weight:bold; }"
            "QPushButton:hover { background:%1; opacity:0.8; border:1px solid #fff; }"
            "QPushButton:pressed { background:#111; }").arg(color));
        return b;
    };

    QPushButton *btnHello = makeBtn("Say Hello",  "#2060c0");
    QPushButton *btnInfo  = makeBtn("About Qt5",  "#6020a0");
    QPushButton *btnReset = makeBtn("Reset",       "#804000");

    QObject::connect(btnHello, &QPushButton::clicked, [=]() {
        QMessageBox::information(nullptr, "Hello from AscentOS",
            "Qt5 is running on AscentOS!\n\nEnjoy the ride.");
    });
    QObject::connect(btnInfo, &QPushButton::clicked, [=]() {
        QMessageBox::aboutQt(nullptr, "About Qt5");
    });
    QObject::connect(btnReset, &QPushButton::clicked, [=]() {
        sl->setValue(0);
    });

    bl->addWidget(btnHello);
    bl->addWidget(btnInfo);
    bl->addWidget(btnReset);
    vl->addWidget(btnGrp);
    vl->addStretch();
    return w;
}

// ── Main window ─────────────────────────────────────────────────────────────
class MainWindow : public QMainWindow {
public:
    MainWindow() {
        setWindowTitle("AscentOS Qt5 Dashboard");
        setMinimumSize(560, 420);

        // Dark palette
        QPalette pal;
        pal.setColor(QPalette::Window,          QColor(18,  22,  38));
        pal.setColor(QPalette::WindowText,       QColor(220, 230, 255));
        pal.setColor(QPalette::Base,             QColor(12,  16,  28));
        pal.setColor(QPalette::AlternateBase,    QColor(24,  28,  48));
        pal.setColor(QPalette::Text,             QColor(220, 230, 255));
        pal.setColor(QPalette::Button,           QColor(30,  36,  60));
        pal.setColor(QPalette::ButtonText,       QColor(220, 230, 255));
        pal.setColor(QPalette::Highlight,        QColor(50, 130, 220));
        pal.setColor(QPalette::HighlightedText,  QColor(255, 255, 255));
        setPalette(pal);

        // Menu bar
        QMenu *fileMenu = menuBar()->addMenu("&File");
        QAction *quitAct = fileMenu->addAction("&Quit");
        quitAct->setShortcut(QKeySequence("Ctrl+Q"));
        connect(quitAct, &QAction::triggered, this, &QMainWindow::close);

        QMenu *helpMenu = menuBar()->addMenu("&Help");
        QAction *aboutAct = helpMenu->addAction("&About");
        connect(aboutAct, &QAction::triggered, [this]() {
            QMessageBox::about(this, "About",
                "<b>AscentOS Qt5 Dashboard</b><br>"
                "Version 1.0<br><br>"
                "A test application demonstrating Qt5 on AscentOS.");
        });

        // Central widget
        QWidget *central = new QWidget;
        QVBoxLayout *cl = new QVBoxLayout(central);
        cl->setSpacing(8);
        cl->setContentsMargins(8, 8, 8, 8);

        // Wave widget
        m_wave = new WaveWidget;
        cl->addWidget(m_wave);

        // Tabs
        QTabWidget *tabs = new QTabWidget;
        tabs->setStyleSheet(
            "QTabWidget::pane { border:1px solid #334; }"
            "QTabBar::tab { background:#1a1e30; color:#aac; padding:5px 14px; border:1px solid #334; }"
            "QTabBar::tab:selected { background:#252a48; color:#fff; }");

        QWidget *infoTab = makeInfoTab();
        m_uptimeLabel = infoTab->findChild<QLabel*>("uptimeLabel");

        tabs->addTab(infoTab,               "System Info");
        tabs->addTab(makeControlsTab(&m_progress), "Controls");

        cl->addWidget(tabs, 1);
        setCentralWidget(central);

        // Status bar
        statusBar()->showMessage("AscentOS Qt5 test app ready.");
        statusBar()->setStyleSheet("background:#12162a; color:#88aacc;");

        // Timer: 50 ms tick
        m_timer = new QTimer(this);
        connect(m_timer, &QTimer::timeout, this, &MainWindow::tick);
        m_timer->start(50);
    }

private slots:
    void tick() {
        m_wave->advance(0.07);

        // Progress bar: auto-fill cycle
        int v = m_progress->value() + 1;
        if (v > 100) v = 0;
        m_progress->setValue(v);

        // Uptime label
        if (m_uptimeLabel) {
            long ms = get_uptime_ms();
            int secs = (int)(ms / 1000);
            int mins = secs / 60;
            int hrs  = mins / 60;
            char buf[32];
            snprintf(buf, sizeof(buf), "%02d:%02d:%02d", hrs, mins % 60, secs % 60);
            m_uptimeLabel->setText(buf);
        }
    }

private:
    WaveWidget   *m_wave        = nullptr;
    QProgressBar *m_progress    = nullptr;
    QLabel       *m_uptimeLabel = nullptr;
    QTimer       *m_timer       = nullptr;
};

// ── Entry point ─────────────────────────────────────────────────────────────
int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    app.setApplicationName("AscentOS Qt5 Dashboard");
    app.setApplicationVersion("1.0");

    MainWindow mw;
    mw.show();

    return app.exec();
}
