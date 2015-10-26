#include "logplot.h"

void LogPlot::run()
{
    LogPlotHandler h;
    BaseScanner* s = getScanner();
    s->pid_handlers.push_back(&h);
    s->fork();
    s->join();
    delete s;
}

LogPlotHandler::LogPlotHandler()
    : reqNumber_(false)
{}

void LogPlotHandler::invoke(logrec_t &r) 
{
        uint4_t pid = r.construct_pid().page;
        if (r.type() == logrec_t::t_page_read) 
            cout << reqNumber_ << " R " << pid << endl;
        else
            cout << reqNumber_ << " U " << pid << endl;
        reqNumber_++;
}

void LogPlotHandler::finalize() 
{}

