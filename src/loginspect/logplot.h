#ifndef LOGPLOT_H
#define LOGPLOT_H

#include "command.h"
#include "handler.h"

class LogPlot : public LogScannerCommand {
public:
    void run();
};

class LogPlotHandler : public Handler {
public:
    LogPlotHandler();
    virtual void invoke(logrec_t& r);
    virtual void finalize();
protected:
    int reqNumber_;
};

#endif
