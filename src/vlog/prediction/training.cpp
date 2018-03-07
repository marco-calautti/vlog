#include <vlog/training.h>
#include <csignal>

std::string makeGenericQuery(Program& p, PredId_t predId, uint8_t predCard) {
    std::string query = p.getPredicateName(predId);
    query += "(";
    for (int i = 0; i < predCard; ++i) {
        query += "V" + to_string(i+1);
        if (i != predCard-1) {
            query += ",";
        }
    }
    query += ")";
    return query;
}

std::pair<std::string, int> makeComplexQuery(Program& p, Literal& l, vector<Substitution>& sub, EDBLayer& db) {
    std::string query = p.getPredicateName(l.getPredicate().getId());
    int card = l.getPredicate().getCardinality();
    query += "(";
    QueryType queryType;
    int countConst = 0;
    for (int i = 0; i < card; ++i) {
        std::string canV = "V" + to_string(i+1);
        //FIXME: uint8_t id = p.getIDVar(canV); //I don't know how to convert this line
        uint8_t id = 0;
        bool found = false;
        for (int j = 0; j < sub.size(); ++j) {
            if (sub[j].origin == id) {
                char supportText[MAX_TERM_SIZE];
                db.getDictText(sub[j].destination.getValue(), supportText);
                query += supportText;
                found = true;
                countConst++;
            }
        }
        if (!found) {
            query += canV;
        }
        if (i != card-1) {
            query += ",";
        }
    }
    query += ")";

    if (countConst == card) {
        queryType = QUERY_TYPE_BOOLEAN;
    } else if (countConst == 0) {
        queryType = QUERY_TYPE_GENERIC;
    } else {
        queryType = QUERY_TYPE_MIXED;
    }
    return std::make_pair(query, queryType);
}

template <typename Generic>
std::vector<std::vector<Generic>> powerset(std::vector<Generic>& set) {
    std::vector<std::vector<Generic>> output;
    uint16_t setSize = set.size();
    uint16_t powersetSize = pow((uint16_t)2, setSize) - 1;
    for (int i = 1; i <= powersetSize; ++i) {
        std::vector<Generic> element;
        for (int j = 0; j < setSize; ++j) {
            if (i & (1<<j)) {
                element.push_back(set[j]);
            }
        }
        output.push_back(element);
    }
    return output;
}

PredId_t getMatchingIDB(EDBLayer& db, Program &p, vector<uint64_t>& tuple) {
    //Check this tuple with all rules
    PredId_t idbPredicateId = 65535;
    vector<Rule> rules = p.getAllRules();
    vector<Rule>::iterator it = rules.begin();
    vector<pair<uint8_t, uint64_t>> ruleTuple;
    for (;it != rules.end(); ++it) {
        vector<Literal> body = (*it).getBody();
        if (body.size() > 1) {
            continue;
        }
        uint8_t nConstants = body[0].getNConstants();
        Predicate temp = body[0].getPredicate();
        if (!p.isPredicateIDB(temp.getId())){
            int matched = 0;
            for (int c = 0; c < temp.getCardinality(); ++c) {
                uint8_t tempid = body[0].getTermAtPos(c).getId();
                if(tempid == 0) {
                    uint64_t tempvalue = body[0].getTermAtPos(c).getValue();
                    char supportText[MAX_TERM_SIZE];
                    db.getDictText(tempvalue, supportText);
                    if (tempvalue == tuple[c]) {
                        matched++;
                    }
                }
            }
            if (matched == nConstants) {
                idbPredicateId = (*it).getFirstHead().getPredicate().getId();
                return idbPredicateId;
            }
        }
    }
    return idbPredicateId;
}

std::vector<std::pair<std::string, int>> Training::generateTrainingQueries(EDBConf &conf,
        EDBLayer &db,
        Program &p,
        int depth,
        uint64_t maxTuples,
        std::vector<uint8_t>& vt
        ) {
    std::unordered_map<string, int> allQueries;

    typedef std::pair<PredId_t, vector<Substitution>> EndpointWithEdge;
    typedef std::unordered_map<uint16_t, std::vector<EndpointWithEdge>> Graph;
    Graph graph;

    std::vector<Rule> rules = p.getAllRules();
    for (int i = 0; i < rules.size(); ++i) {
        Rule ri = rules[i];
        Predicate ph = ri.getFirstHead().getPredicate();
        std::vector<Substitution> sigmaH;
        for (int j = 0; j < ph.getCardinality(); ++j) {
            VTerm dest = ri.getFirstHead().getTuple().get(j);
            sigmaH.push_back(Substitution(vt[j], dest));
        }
        std::vector<Literal> body = ri.getBody();
        for (std::vector<Literal>::const_iterator itr = body.begin(); itr != body.end(); ++itr) {
            Predicate pb = itr->getPredicate();
            std::vector<Substitution> sigmaB;
            for (int j = 0; j < pb.getCardinality(); ++j) {
                VTerm dest = itr->getTuple().get(j);
                sigmaB.push_back(Substitution(vt[j], dest));
            }
            // Calculate sigmaB * sigmaH
            std::vector<Substitution> edge_label = inverse_concat(sigmaB, sigmaH);
            EndpointWithEdge neighbour = std::make_pair(ph.getId(), edge_label);
            graph[pb.getId()].push_back(neighbour);
        }
    }

#if DEBUG
    // Try printing graph
    for (auto it = graph.begin(); it != graph.end(); ++it) {
        uint16_t id = it->first;
        std::cout << p.getPredicateName(id) << " : " << std::endl;
        std::vector<EndpointWithEdge> nei = it->second;
        for (int i = 0; i < nei.size(); ++i) {
            Predicate pred = p.getPredicate(nei[i].first);
            std::vector<Substitution> sub = nei[i].second;
            for (int j = 0; j < sub.size(); ++j){
                std::cout << p.getPredicateName(nei[i].first) << "{" << sub[j].origin << "->"
                    << sub[j].destination.getId() << " , " << sub[j].destination.getValue() << "}" << std::endl;
            }
        }
        std::cout << "=====" << std::endl;
    }
#endif

    // Gather all predicates
    std::vector<PredId_t> ids = p.getAllEDBPredicateIds();
    std::ofstream allPredicatesLog("allPredicatesInQueries.log");
    Dictionary dictVariables;
    for (int i = 0; i < ids.size(); ++i) {
        int neighbours = graph[ids[i]].size();
        LOG(INFOL) << p.getPredicateName(ids[i]) << " is EDB : " << neighbours << "neighbours";
        Predicate edbPred = p.getPredicate(ids[i]);
        int card = edbPred.getCardinality();
        std::string query = makeGenericQuery(p, edbPred.getId(), edbPred.getCardinality());
        Literal literal = p.parseLiteral(query, dictVariables);
        int nVars = literal.getNVars();
        QSQQuery qsqQuery(literal);
        TupleTable *table = new TupleTable(nVars);
        db.query(&qsqQuery, table, NULL, NULL);
        uint64_t nRows = table->getNRows();
        std::vector<std::vector<uint64_t>> output;
        /**
         * RP1(A,B) :- TE(A, <studies>, B)
         * RP2(A,B) :- TE(A, <worksFor>, B)
         *
         * Tuple <jon, studies, VU> can match with RP2, which it should not
         *
         * All EDB tuples should be carefully matched with rules
         * */
        PredId_t predId = edbPred.getId();
        uint64_t rowNumber = 0;
        if (maxTuples > nRows) {
            maxTuples = nRows;
        }
        while (rowNumber < maxTuples) {
            std::vector<uint64_t> tuple;
            std::string tupleString("<");
            for (int j = 0; j < nVars; ++j) {
                uint64_t value = table->getPosAtRow(rowNumber, j);
                tuple.push_back(value);
                char supportText[MAX_TERM_SIZE];
                db.getDictText(value, supportText);
                tupleString += supportText;
                tupleString += ",";
            }
            tupleString += ">";
            LOG(INFOL) << "Tuple # " << rowNumber << " : " << tupleString;
            PredId_t idbPredId = getMatchingIDB(db, p, tuple);
            if (65535 == idbPredId) {
                rowNumber++;
                continue;
            }
            std::string predName = p.getPredicateName(idbPredId);

            LOG(INFOL) << tupleString << " ==> " << predName << " : " << +idbPredId;
            vector<Substitution> subs;
            for (int k = 0; k < card; ++k) {
                subs.push_back(Substitution(vt[k], VTerm(0, tuple[k])));
            }
            // Find powerset of subs here
            std::vector<std::vector<Substitution>> options =  powerset<Substitution>(subs);
            unsigned int seed = (unsigned int) ((clock() ^ 413711) % 105503);
            srand(seed);
            for (int l = 0; l < options.size(); ++l) {
                vector<Substitution> sigma = options[l];
                PredId_t predId = edbPred.getId();
                int n = 1;
                while (n != depth+1) {
                    uint32_t nNeighbours = graph[predId].size();
                    if (0 == nNeighbours) {
                        break;
                    }
                    uint32_t randomNeighbour;
                    if (1 == n) {
                        int index = 0;
                        bool found = false;
                        for (auto it = graph[predId].begin(); it != graph[predId].end(); ++it,++index) {
                            if (it->first == idbPredId) {
                                randomNeighbour = index;
                                found = true;
                                break;
                            }
                        }
                        assert(found == true);
                    } else {
                        randomNeighbour = rand() % nNeighbours;
                    }
                    std::vector<Substitution>sigmaN = graph[predId][randomNeighbour].second;
                    std::vector<Substitution> result = concat(sigmaN, sigma);
                    PredId_t qId  = graph[predId][randomNeighbour].first;
                    uint8_t qCard = p.getPredicate(graph[predId][randomNeighbour].first).getCardinality();
                    std::string qQuery = makeGenericQuery(p, qId, qCard);
                    Literal qLiteral = p.parseLiteral(qQuery, dictVariables);
                    allPredicatesLog << p.getPredicateName(qId) << std::endl;
                    std::pair<string, int> finalQueryResult = makeComplexQuery(p, qLiteral, result, db);
                    std::string qFinalQuery = finalQueryResult.first;
                    int type = finalQueryResult.second + ((n > 4) ? 4 : n);
                    if (allQueries.find(qFinalQuery) == allQueries.end()) {
                        allQueries.insert(std::make_pair(qFinalQuery, type));
                    }

                    predId = qId;
                    sigma = result;
                    n++;
                } // while the depth of exploration is reached
            } // for each partial substitution
            rowNumber++;
        }
    } // all EDB predicate ids
    allPredicatesLog.close();
    std::vector<std::pair<std::string,int>> queries;
    for (std::unordered_map<std::string,int>::iterator it = allQueries.begin(); it !=  allQueries.end(); ++it) {
        queries.push_back(std::make_pair(it->first, it->second));
        LOG(INFOL) << "Query: " << it->first << " type : " << it->second ;
    }
    return queries;
}
pid_t pid;
bool timedOut;
void alarmHandler(int signalNumber) {
    if (signalNumber == SIGALRM) {
        kill(pid, SIGKILL);
        timedOut = true;
    }
}

double Training::runAlgo(string& algo,
        Reasoner& reasoner,
        EDBLayer& edb,
        Program& p,
        Literal& literal,
        stringstream& ss,
        uint64_t timeoutMillis) {

    int ret;

    std::chrono::duration<double> durationQuery;
    signal(SIGALRM, alarmHandler);
    timedOut = false;

    double* queryTime = (double*) mmap(NULL, sizeof(double), PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS, -1, 0);

    pid = fork();
    if (pid < 0) {
        LOG(ERRORL) << "Could not fork";
        return 0.0L;
    } else if (pid == 0) {
        //Child work begins
        //+
        std::chrono::system_clock::time_point queryStartTime = std::chrono::system_clock::now();
        //int times = vm["repeatQuery"].as<int>();
        bool printResults = false; // vm["printResults"].as<bool>();

        int nVars = literal.getNVars();
        bool onlyVars = nVars > 0;

        TupleIterator *iter;
        if (algo == "magic"){
            iter = reasoner.getMagicIterator(literal, NULL, NULL, edb, p, onlyVars, NULL);
        } else if (algo == "qsqr") {
            iter = reasoner.getTopDownIterator(literal, NULL, NULL, edb, p, onlyVars, NULL);
        } else {
            LOG(ERRORL) << "Algorithm not supported : " << algo;
            return 0;
        }
        long count = 0;
        int sz = iter->getTupleSize();
        if (nVars == 0) {
            ss << (iter->hasNext() ? "TRUE" : "FALSE") << endl;
            count = (iter->hasNext() ? 1 : 0);
        } else {
            while (iter->hasNext()) {
                iter->next();
                count++;
                if (printResults) {
                    for (int i = 0; i < sz; i++) {
                        char supportText[MAX_TERM_SIZE];
                        uint64_t value = iter->getElementAt(i);
                        if (i != 0) {
                            ss << " ";
                        }
                        if (!edb.getDictText(value, supportText)) {
                            LOG(ERRORL) << "Term " << value << " not found";
                        } else {
                            ss << supportText;
                        }
                    }
                    ss << endl;
                }
            }
        }
        std::chrono::system_clock::time_point queryEndTime = std::chrono::system_clock::now();
        durationQuery = queryEndTime - queryStartTime;
        LOG(INFOL) << "QueryTime = " << durationQuery.count();
        *queryTime = durationQuery.count()*1000;
        exit(0);
        //-
        //Child work ends
    } else {
        uint64_t l =  timeoutMillis / 1000;
        alarm(l);
        int status;
        ret = waitpid(pid, &status, 0);
        alarm(0);
        if (timedOut) {
            LOG(INFOL) << "TIMED OUTTTTT";
            munmap(queryTime, sizeof(double));
            return timeoutMillis;
        }
        double time = *queryTime;
        LOG(INFOL) << "query time in parent :" << time;
        munmap(queryTime, sizeof(double));
        return time;
    }
}

void Training::execLiteralQuery(string& literalquery,
        EDBLayer& edb,
        Program& p,
        bool jsonoutput,
        JSON* jsonResults,
        JSON* jsonFeatures,
        JSON* jsonQsqrTime,
        JSON* jsonMagicTime) {

    Dictionary dictVariables;
    Literal literal = p.parseLiteral(literalquery, dictVariables);
    Reasoner reasoner(1000000);

    Metrics metrics;
    reasoner.getMetrics(literal, NULL, NULL, edb, p, metrics, 5);
    stringstream strMetrics;
    strMetrics  << std::to_string(metrics.cost) << ","
        << std::to_string(metrics.estimate) << ", "
        << std::to_string(metrics.countRules) << ", "
        << std::to_string(metrics.countUniqueRules) << ", "
        << std::to_string(metrics.countIntermediateQueries) << ", "
        << std::to_string(metrics.countIDBPredicates);
    jsonFeatures->put("features", strMetrics.str());

    stringstream ssQsqr;
    string algo = "qsqr";
    double durationQsqr = Training::runAlgo(algo, reasoner, edb, p, literal, ssQsqr, 5000);

    stringstream ssMagic;
    algo = "magic";
    double durationMagic = Training::runAlgo(algo, reasoner, edb, p, literal, ssMagic, 5000);
    LOG(INFOL) << "Qsqr time : " << durationQsqr;
    LOG(INFOL) << "magic time: " << durationMagic;
    jsonResults->put("results", ssQsqr.str());
    jsonQsqrTime->put("qsqrtime", to_string(durationQsqr));
    jsonMagicTime->put("magictime", to_string(durationMagic));
}
