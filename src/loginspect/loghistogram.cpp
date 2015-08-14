#include "loghistogram.h"

void LogHistogram::setupOptions()
{
    LogScannerCommand::setupOptions();
    options.add_options()
        ("histogram,t",  po::value<bool>(&optHistogram_)->default_value(false)
            ->implicit_value(true),"Create data to Histogram Graph")
        ("boxplots,b", po::value<bool>(&optBoxplots_)->default_value(false)
            ->implicit_value(true), "Create data to Boxplots")
    ;
}

void LogHistogram::run()
{
    HistogramHandler h;
    h.setHistogram(optHistogram_);
    h.setBoxplots(optBoxplots_);

    BaseScanner* s = getScanner();
    s->pid_handlers.push_back(&h);
    s->fork();
    s->join();
    delete s;
}

HistogramHandler::HistogramHandler()
    : histogram_(false),
      boxplots_(false)
{}

void HistogramHandler::invoke(logrec_t &r) 
{
    uint4_t pid = r.construct_pid().page;
     countAccess_[pid]++;
}

void HistogramHandler::finalize() 
{
    std::vector<int> vectorCountAccess;
    long unsigned int acc = 0;
    
    for (std::map<unsigned int,int>::iterator it=countAccess_.begin(); it!=countAccess_.end(); ++it)
        vectorCountAccess.push_back(it->second);
    std::sort(vectorCountAccess.begin(), vectorCountAccess.end());
    std::reverse(vectorCountAccess.begin(), vectorCountAccess.end());
    if (histogram_) {
        long unsigned int i;
        long int subDivision = vectorCountAccess.size()/20;
        int aux=100;
        for (long unsigned int j = 0; j < vectorCountAccess.size();) {
            for (i = j; i < j + subDivision && i < vectorCountAccess.size(); i++) {
                acc = vectorCountAccess.at(i) + acc;
            }
            cout << "\"" << aux << "% - " << aux-5 << "%\"\t" << acc << endl;
            j = i;
            acc = 0;
            aux = aux - 5;
        }
    }
    else if(boxplots_) {
        long int totalAccesses = 0, quarters[3];
        for (long unsigned int j = 0; j < vectorCountAccess.size() - 20; j++) {
                totalAccesses = vectorCountAccess.at(j) + totalAccesses;
        }
        int quarterDefiner=4;
        float delimiterQuarter = 0.25;
        totalAccesses = acc;
        long int acc = 0;
        for (long unsigned int j = 0; j < vectorCountAccess.size(); j++) {
                acc = vectorCountAccess.at(j) + acc;
                if  (acc >= totalAccesses*delimiterQuarter) {
                    switch (quarterDefiner) {
                        case 4:
                            quarters[0] = j;
                            quarterDefiner = 2;
                            delimiterQuarter = 0.5;
                            break;
                        case 2:
                            quarters[1] = j;
                            quarterDefiner = 1;
                            delimiterQuarter = 0.75;
                            break;
                        case 1:
                            quarters[2] = j;
                            //stops loop
                            delimiterQuarter = 1.5;
                            break;
                    }

                }
        }
        cout << 0 << "\t" << quarters[0] << "\t" << quarters[1] << "\t" << quarters[2] << "\t" << vectorCountAccess.size() << endl;
    }
    else
        for (long unsigned int i = 0; i <  vectorCountAccess.size(); i++) {
            cout << vectorCountAccess.at(i) << " ";
            cout << endl;
        }


}

void HistogramHandler::setHistogram(bool histogram)
{
    histogram_ = histogram;
}

void HistogramHandler::setBoxplots(bool boxplots)
{
    boxplots_ = boxplots;
}



