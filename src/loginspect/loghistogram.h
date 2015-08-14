#ifndef LOGHISTOGRAM_H
#define LOGHISTOGRAM_H

#include "command.h"
#include "handler.h"

#include <vector>
#include <map>

class LogHistogram : public LogScannerCommand {
public:
    void run();
    void setupOptions();
protected:
    bool  optHistogram_, optBoxplots_;
};

class HistogramHandler : public Handler {
public:
    HistogramHandler();
    void setHistogram(bool histogram);
    void setBoxplots(bool boxplots);
    virtual void invoke(logrec_t& r);
    virtual void finalize();
protected:
    std::map <uint4_t, int> countAccess_;
    bool finalized_, histogram_, boxplots_;
};

#endif
