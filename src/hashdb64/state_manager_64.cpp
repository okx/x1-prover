#include "state_manager_64.hpp"
#include "zkassert.hpp"
#include "zklog.hpp"
#include "scalar.hpp"
#include "timer.hpp"
#include "persistence.hpp"
#include "definitions.hpp"

StateManager64 stateManager64;

zkresult StateManager64::setStateRoot (const string &batchUUID, uint64_t tx, const string &_stateRoot, bool bIsOldStateRoot, const Persistence persistence)
{
#ifdef LOG_TIME_STATISTICS_STATE_MANAGER
    struct timeval t;
    gettimeofday(&t, NULL);
#endif

    // Normalize state root format
    string stateRoot = NormalizeToNFormat(_stateRoot, 64);
    stateRoot = stringToLower(stateRoot);

    // Check persistence range
    if (persistence >= PERSISTENCE_SIZE)
    {
        zklog.error("StateManager64::setStateRoot() invalid persistence batchUUID=" + batchUUID + " tx=" + to_string(tx) + " stateRoot=" + stateRoot + " bIsOldStateRoot=" + to_string(bIsOldStateRoot) + " persistence=" + persistence2string(persistence));
        return ZKR_STATE_MANAGER;
    }

#ifdef LOG_STATE_MANAGER
    zklog.info("StateManager64::setStateRoot() batchUUID=" + batchUUID + " tx=" + to_string(tx) + " stateRoot=" + stateRoot + " bIsOldStateRoot=" + to_string(bIsOldStateRoot) + " persistence=" + persistence2string(persistence));
#endif

    Lock();

    unordered_map<string, BatchState64>::iterator it;

    // Find batch state for this uuid, or create it if it does not exist
    it = state.find(batchUUID);
    if (it == state.end())
    {
        if (!bIsOldStateRoot)
        {
            zklog.error("StateManager64::setStateRoot() called with bIsOldStateRoot=false, but batchUUID=" + batchUUID + " does not previously exist");
            Unlock();
            return ZKR_STATE_MANAGER;
        }
        BatchState64 batchState;
        batchState.oldStateRoot = stateRoot;
        state[batchUUID] = batchState;
        it = state.find(batchUUID);
        zkassert(it != state.end());
    }
    BatchState64 &batchState = it->second;

    // Set the current state root
    batchState.currentStateRoot = stateRoot;

    // Create tx states, if needed
    if (tx >= batchState.txState.size())
    {
        // If this is the first state of a new tx, check that it is the old state root
        if (!bIsOldStateRoot)
        {
            zklog.error("StateManager64::setStateRoot() called with bIsOldStateRoot=false, but tx=" + to_string(tx) + " does not previously exist");
            Unlock();
            return ZKR_STATE_MANAGER;
        }

        // Calculate the number of tx slots to create
        uint64_t txsToCreate = tx - batchState.txState.size() + 1;

        // Create TX state to insert
        TxState64 txState;
        
        // Insert TX state
        for (uint64_t i=0; i<txsToCreate; i++)
        {
            batchState.txState.emplace_back(txState);
        }

        // Set current TX
        batchState.currentTx = tx;
    }

    // Get a reference to the tx state
    TxState64 &txState = batchState.txState[tx];

    // Get the current sub-state list size
    uint64_t currentSubStateSize = txState.persistence[persistence].subState.size();
    
    // In case it is an old state root, we need to create a new sub-state, and check that everything makes sense
    if (bIsOldStateRoot)
    {
        // If this is the first sub-state of the tx state, record the tx old state root
        if ( currentSubStateSize == 0)
        {
            // Check current sub-state
            if (txState.persistence[persistence].currentSubState != 0)
            {
                zklog.error("StateManager64::setStateRoot() currentSubState=" + to_string(txState.persistence[persistence].currentSubState) + "!=0 batchUUID=" + batchUUID + " tx=" + to_string(tx) + " stateRoot=" + stateRoot + " bIsOldStateRoot=" + to_string(bIsOldStateRoot) + " persistence=" + persistence2string(persistence));
                Unlock();
                return ZKR_STATE_MANAGER;
            }

            // Record the old state root
            txState.persistence[persistence].oldStateRoot = stateRoot;
        }

        // If it is not the first sub-state, it must have been called with the previous new state root
        else
        {
            // Check current sub-state
            if (txState.persistence[persistence].currentSubState >= currentSubStateSize)
            {
                zklog.error("StateManager64::setStateRoot() currentSubState=" + to_string(txState.persistence[persistence].currentSubState) + " > currentSubStateSize=" + to_string(currentSubStateSize) + " batchUUID=" + batchUUID + " tx=" + to_string(tx) + " stateRoot=" + stateRoot + " bIsOldStateRoot=" + to_string(bIsOldStateRoot) + " persistence=" + persistence2string(persistence));
                Unlock();
                return ZKR_STATE_MANAGER;
            }

            // Check that new state root is empty
            if (txState.persistence[persistence].subState[currentSubStateSize-1].newStateRoot.size() == 0)
            {
                zklog.error("StateManager64::setStateRoot() oldStateRoot found previous newStateRoot empty");
                Unlock();
                return ZKR_STATE_MANAGER;
            }
        }

        // Create TX sub-state
        TxSubState64 txSubState;
        txSubState.oldStateRoot = stateRoot;
        txSubState.previousSubState = txState.persistence[persistence].currentSubState;

        // Insert it
        txState.persistence[persistence].subState.emplace_back(txSubState);

        // Record the current state
        txState.persistence[persistence].currentSubState = txState.persistence[persistence].subState.size() - 1;
    }

    // If it is a new state root, we need to complete the current sub-state
    else
    {
        if (txState.persistence[persistence].currentSubState >= currentSubStateSize)
        {
            zklog.error("StateManager64::setStateRoot() currentSubState=" + to_string(txState.persistence[persistence].currentSubState) + " > currentSubStateSize=" + to_string(currentSubStateSize) + " batchUUID=" + batchUUID + " tx=" + to_string(tx) + " stateRoot=" + stateRoot + " bIsOldStateRoot=" + to_string(bIsOldStateRoot) + " persistence=" + persistence2string(persistence));
            Unlock();
            return ZKR_STATE_MANAGER;
        }

        // Check that the new state root is empty
        if (txState.persistence[persistence].subState[txState.persistence[persistence].currentSubState].newStateRoot.size() != 0)
        {
            zklog.error("StateManager64::setStateRoot() found nesStateRoot busy");
            Unlock();
            return ZKR_STATE_MANAGER;
        }

        // Record the new state root in the tx sub-state, and in the tx state
        txState.persistence[persistence].subState[txState.persistence[persistence].currentSubState].newStateRoot = stateRoot;
        txState.persistence[persistence].newStateRoot = stateRoot;
    }

#ifdef LOG_TIME_STATISTICS_STATE_MANAGER
    timeMetricStorage.add("setStateRoot", TimeDiff(t));
#endif

    Unlock();

    return ZKR_SUCCESS;

}

zkresult StateManager64::write (const string &batchUUID, uint64_t tx, const string &_key, const string &value, const Persistence persistence)
{
#ifdef LOG_TIME_STATISTICS_STATE_MANAGER
    struct timeval t;
    gettimeofday(&t, NULL);
#endif

    // Normalize key format
    string key = NormalizeToNFormat(_key, 64);
    key = stringToLower(key);

#ifdef LOG_STATE_MANAGER
    zklog.info("StateManager64::write() batchUUID=" + batchUUID + " tx=" + to_string(tx) + " key=" + key + " persistence=" + persistence2string(persistence));
#endif

    // Check persistence range
    if (persistence >= PERSISTENCE_SIZE)
    {
        zklog.error("StateManager64::write() wrong persistence batchUUID=" + batchUUID + " tx=" + to_string(tx) + " key=" + key + " persistence=" + persistence2string(persistence));
        return ZKR_STATE_MANAGER;
    }

    Lock();

    // Find batch state for this uuid
    unordered_map<string, BatchState64>::iterator it;
    it = state.find(batchUUID);
    if (it == state.end())
    {
        zklog.error("StateManager64::write() found no batch state for batch UUID=" + batchUUID);
        Unlock();
        return ZKR_STATE_MANAGER;
    }
    BatchState64 &batchState = it->second;

    // Check tx range
    if (tx > batchState.txState.size())
    {
        zklog.error("StateManager64::write() got tx=" + to_string(tx) + " bigger than txState size=" + to_string(it->second.txState.size()));
        Unlock();
        return ZKR_STATE_MANAGER;
    }

    // Create TxState, if not existing
    if (tx == batchState.txState.size())
    {
        TxState64 aux;
        aux.persistence[persistence].oldStateRoot = it->second.currentStateRoot;
        it->second.txState.emplace_back(aux);
    }
    TxState64 &txState = batchState.txState[tx];

    // Create TxSubState, if not existing
    if (txState.persistence[persistence].subState.size() == 0)
    {
        TxSubState64 subState;
        subState.previousSubState = 0;
        subState.oldStateRoot = batchState.currentStateRoot;
        txState.persistence[persistence].subState.emplace_back(subState);
        txState.persistence[persistence].currentSubState = 0;
    }

    // Add to sub-state
    txState.persistence[persistence].subState[txState.persistence[persistence].currentSubState].dbWrite[key] = value;
    
    // Add to common write pool to speed up read
    batchState.dbWrite[key] = value;

#ifdef LOG_TIME_STATISTICS_STATE_MANAGER
    timeMetricStorage.add("write", TimeDiff(t));
#endif

    Unlock();

    return ZKR_SUCCESS;
}

zkresult StateManager64::deleteNode (const string &batchUUID, uint64_t tx, const string &_key, const Persistence persistence)
{
#ifdef LOG_TIME_STATISTICS_STATE_MANAGER
    struct timeval t;
    gettimeofday(&t, NULL);
#endif

    // Normalize key format
    string key = NormalizeToNFormat(_key, 64);
    key = stringToLower(key);

#ifdef LOG_STATE_MANAGER
    zklog.info("StateManager64::deleteNode() batchUUID=" + batchUUID + " tx=" + to_string(tx) + " key=" + key + " persistence=" + persistence2string(persistence));
#endif

    // Check persistence range
    if (persistence >= PERSISTENCE_SIZE)
    {
        zklog.error("StateManager64::deleteNode() invalid persistence batchUUID=" + batchUUID + " tx=" + to_string(tx) + " key=" + key + " persistence=" + persistence2string(persistence));
        return ZKR_STATE_MANAGER;
    }

    Lock();

    unordered_map<string, BatchState64>::iterator it;

    // Find batch state for this batch uuid
    it = state.find(batchUUID);
    if (it == state.end())
    {
        zklog.error("StateManager64::deleteNode() found no batch state for batch UUID=" + batchUUID);
        Unlock();
        return ZKR_STATE_MANAGER;
    }
    BatchState64 &batchState = it->second;

    // Check tx range
    if (tx >= batchState.txState.size())
    {
        zklog.error("StateManager64::deleteNode() got tx=" + to_string(tx) + " bigger than txState size=" + to_string(it->second.txState.size()));
        Unlock();
        return ZKR_STATE_MANAGER;
    }

    // Find TX state for this tx
    TxState64 &txState = batchState.txState[tx];
    
    // Find TX current sub-state
    if (txState.persistence[persistence].subState.size() == 0)
    {
        zklog.error("StateManager64::deleteNode() found subState.size=0 tx=" + to_string(tx) + " batchUUIDe=" + batchUUID);
        Unlock();
        return ZKR_STATE_MANAGER;
    }
    if (txState.persistence[persistence].currentSubState >= txState.persistence[persistence].subState.size())
    {
        zklog.error("StateManager64::deleteNode() found currentSubState=" + to_string(txState.persistence[persistence].currentSubState) + " >= subState.size=" + to_string(txState.persistence[persistence].subState.size()) + " tx=" + to_string(tx) + " batchUUIDe=" + batchUUID);
        Unlock();
        return ZKR_STATE_MANAGER;
    }
    TxSubState64 &txSubState = txState.persistence[persistence].subState[txState.persistence[persistence].currentSubState];

    // Delete this key in the surrent TX sub-state
    /*unordered_map<string, vector<Goldilocks::Element>>::iterator dbIt;
    dbIt = txSubState.dbWrite.find(key);
    if (dbIt != txSubState.dbWrite.end())
    {
        txSubState.dbWrite.erase(dbIt);
        zklog.info("StateManager64::deleteNode() batchUUID=" + batchUUID + " tx=" + to_string(tx) + " key=" + key);
    }*/

    txSubState.dbDelete.emplace_back(key);

#ifdef LOG_TIME_STATISTICS_STATE_MANAGER
    timeMetricStorage.add("deleteNodes", TimeDiff(t));
#endif

    Unlock();

    return ZKR_SUCCESS;
}

zkresult StateManager64::read (const string &batchUUID, const string &_key, string &value, DatabaseMap *dbReadLog)
{
    struct timeval t;
    gettimeofday(&t, NULL);

    // Normalize key format
    string key = NormalizeToNFormat(_key, 64);
    key = stringToLower(key);

    Lock();

    // Find batch state for this uuid
    unordered_map<string, BatchState64>::iterator it;
    it = state.find(batchUUID);
    if (it == state.end())
    {
        //zklog.error("StateManager64::read() found no batch state for batch UUID=" + batchUUID);
        Unlock();
        return ZKR_DB_KEY_NOT_FOUND;
    }
    BatchState64 &batchState = it->second;

    // Search in the common write list
    unordered_map<string, string>::iterator dbIt;
    dbIt = batchState.dbWrite.find(key);
    if (dbIt != batchState.dbWrite.end())
    {
        value = dbIt->second;
                        
        // Add to the read log
        if (dbReadLog != NULL) dbReadLog->add(key, value, true, TimeDiff(t));

#ifdef LOG_STATE_MANAGER
        zklog.info("StateManager64::read() batchUUID=" + batchUUID + " key=" + key);
#endif

#ifdef LOG_TIME_STATISTICS_STATE_MANAGER
        timeMetricStorage.add("read success", TimeDiff(t));
#endif
        Unlock();

        return ZKR_SUCCESS;
    }

#ifdef LOG_TIME_STATISTICS_STATE_MANAGER
    timeMetricStorage.add("read not found", TimeDiff(t));
#endif

    Unlock();

    return ZKR_DB_KEY_NOT_FOUND;
}

bool IsInvalid(TxSubState64 &txSubState)
{
    return !txSubState.bValid;
}

zkresult StateManager64::semiFlush (const string &batchUUID, const string &_stateRoot, const Persistence persistence)
{
#ifdef LOG_TIME_STATISTICS_STATE_MANAGER
    struct timeval t;
    gettimeofday(&t, NULL);
#endif

    // Normalize state root format
    string stateRoot = NormalizeToNFormat(_stateRoot, 64);
    stateRoot = stringToLower(stateRoot);

    // Check persistence range
    if (persistence >= PERSISTENCE_SIZE)
    {
        zklog.error("StateManager64::semiFlush() invalid persistence batchUUID=" + batchUUID + " stateRoot=" + stateRoot + " persistence=" + persistence2string(persistence));
        return ZKR_STATE_MANAGER;
    }

#ifdef LOG_STATE_MANAGER
    zklog.info("StateManager64::semiFlush() batchUUID=" + batchUUID + " stateRoot=" + stateRoot + " persistence=" + persistence2string(persistence));
#endif

    Lock();

    unordered_map<string, BatchState64>::iterator it;

    // Find batch state for this uuid
    it = state.find(batchUUID);
    if (it == state.end())
    {
        zklog.warning("StateManager64::semiFlush() found no batch state for batch UUID=" + batchUUID + "; normal if no SMT activity happened");
 
#ifdef LOG_TIME_STATISTICS_STATE_MANAGER
        timeMetricStorage.add("semiFlush UUID not found", TimeDiff(t));
        timeMetricStorage.print("State Manager calls");
#endif
        Unlock();
        return ZKR_SUCCESS;
    }
    BatchState64 &batchState = it->second;

    // Check currentTx range
    if (batchState.currentTx >= batchState.txState.size())
    {
        zklog.error("StateManager64::semiFlush() found batchState.currentTx=" + to_string(batchState.currentTx) + " >= batchState.txState.size=" + to_string(batchState.txState.size()) + " batchUUID=" + batchUUID + " stateRoot=" + stateRoot + " persistence=" + persistence2string(persistence));
        Unlock();
        return ZKR_STATE_MANAGER;
    }

    // Get a reference to the tx state
    TxState64 &txState = batchState.txState[batchState.currentTx];
    TxPersistenceState64 &txPersistenceState = txState.persistence[persistence];

    if (txPersistenceState.newStateRoot == stateRoot)
    {
        // This is the expected case
    }
    else if (txPersistenceState.oldStateRoot == stateRoot)
    {
        if (config.stateManagerPurge)
        {
            // The TX ended up with the same state root as the beginning, so we can delete all data
            txPersistenceState.subState.clear();
            txPersistenceState.newStateRoot = stateRoot;
            txPersistenceState.currentSubState = 0;
        }
    }
    else
    {
        if (config.stateManagerPurge)
        {
            // Search for the point at which we reach this state, and delete the rest
            bool bFound = false;
            uint64_t i=0;
            uint64_t subStateSize = txPersistenceState.subState.size();
            for (i=0; i<subStateSize; i++)
            {
                if (!bFound && txPersistenceState.subState[i].oldStateRoot == stateRoot)
                {
                    bFound = true;
                    break;
                }
            }
            if (bFound)
            {
                txPersistenceState.newStateRoot = stateRoot;
                txPersistenceState.currentSubState = (i == 0) ? 0 : i-1;
                for (; i<subStateSize; i++)
                {
                    txPersistenceState.subState.pop_back();
                }
            }
        }
    }
    
#ifdef LOG_TIME_STATISTICS_STATE_MANAGER
    timeMetricStorage.add("semi flush", TimeDiff(t));
#endif

    Unlock();

    return ZKR_SUCCESS;
}

zkresult StateManager64::flush (const string &batchUUID, Database64 &db, uint64_t &flushId, uint64_t &lastSentFlushId)
{
#ifdef LOG_TIME_STATISTICS_STATE_MANAGER
    struct timeval t;
    gettimeofday(&t, NULL);
#endif

    TimerStart(STATE_MANAGER_FLUSH);

#ifdef LOG_STATE_MANAGER
    zklog.info("StateManager64::flush() batchUUID=" + batchUUID);
#endif

    // For every TX, track backwards from newStateRoot to oldStateRoot, marking sub-states as valid

    Lock();

    zkresult zkr;

    // Find batch state for this uuid
    unordered_map<string, BatchState64>::iterator it;
    it = state.find(batchUUID);
    if (it == state.end())
    {
        //zklog.warning("StateManager64::flush() found no batch state for batch UUID=" + batchUUID + "; normal if no SMT activity happened");
 
        zkr = db.flush(flushId, lastSentFlushId);
        if (zkr != ZKR_SUCCESS)
        {
            zklog.error("StateManager64::flush() failed calling db.flush() result=" + zkresult2string(zkr));
        }

        TimerStopAndLog(STATE_MANAGER_FLUSH);

#ifdef LOG_TIME_STATISTICS_STATE_MANAGER
        timeMetricStorage.add("flush UUID not found", TimeDiff(t));
        timeMetricStorage.print("State Manager calls");
#endif
        Unlock();
        return zkr;
    }
    BatchState64 &batchState = it->second;

    // For all txs, purge the data to write
    for (uint64_t tx=0; tx<batchState.txState.size(); tx++)
    {
        for (uint64_t persistence = 0; persistence < PERSISTENCE_SIZE; persistence++)
        {
            TxState64 &txState = batchState.txState[tx];

            // Temporary data can be deleted at the end of a batch
            if (persistence == PERSISTENCE_TEMPORARY)
            {
                txState.persistence[persistence].subState.clear();
                txState.persistence[persistence].currentSubState = 0;
                continue;
            }

            // If there's no data, there's nothing to do
            if (txState.persistence[persistence].subState.size() == 0)
            {
                continue;
            }

            // Check that current sub-state newStateRoot matches the TX one
            if (txState.persistence[persistence].subState[txState.persistence[persistence].currentSubState].newStateRoot != txState.persistence[persistence].newStateRoot)
            {
                zklog.error("StateManager64::flush() found inconsistent new state roots: batchUUID=" + batchUUID +
                    " tx=" + to_string(tx) + " txState.newStateRoot=" + txState.persistence[persistence].newStateRoot +
                    " currentSubState=" + to_string(txState.persistence[persistence].currentSubState) +
                    " substate.newStateRoot=" + txState.persistence[persistence].subState[txState.persistence[persistence].currentSubState].newStateRoot);
                     
#ifdef LOG_TIME_STATISTICS_STATE_MANAGER
                timeMetricStorage.add("flush UUID inconsistent new state roots", TimeDiff(t));
                timeMetricStorage.print("State Manager calls");
#endif
                Unlock();
                return ZKR_STATE_MANAGER;
            }

            uint64_t currentSubState = txState.persistence[persistence].currentSubState;
            while (true)
            {
                txState.persistence[persistence].subState[currentSubState].bValid = true;
                if (currentSubState == 0)
                {
                    if (txState.persistence[persistence].subState[currentSubState].oldStateRoot != txState.persistence[persistence].oldStateRoot)
                    {
                        zklog.error("StateManager64::flush() found inconsistent old state roots: batchUUID=" + batchUUID +
                            " tx=" + to_string(tx) + " txState.oldStateRoot=" + txState.persistence[persistence].oldStateRoot +
                            " currentSubState=" + to_string(txState.persistence[persistence].currentSubState) +
                            " substate.oldStateRoot=" + txState.persistence[persistence].subState[currentSubState].oldStateRoot);

#ifdef LOG_TIME_STATISTICS_STATE_MANAGER
                        timeMetricStorage.add("flush UUID inconsistent old state roots", TimeDiff(t));
                        timeMetricStorage.print("State Manager calls");
#endif
                        Unlock();
                        return ZKR_STATE_MANAGER;
                    }
                    break;
                }
                uint64_t previousSubState = txState.persistence[persistence].subState[currentSubState].previousSubState;
                if (txState.persistence[persistence].subState[previousSubState].newStateRoot == txState.persistence[persistence].subState[currentSubState].oldStateRoot)
                {
                    currentSubState = previousSubState;
                    continue;
                }

                // Search for the previous state
                uint64_t i=0;
                for (; i<currentSubState; i++)
                {
                    if (txState.persistence[persistence].subState[i].newStateRoot == txState.persistence[persistence].subState[currentSubState].oldStateRoot)
                    {
                        previousSubState = i;
                        break;
                    }
                }
                if (i == currentSubState)
                {
                    zklog.error("StateManager64::flush() could not find previous tx sub-state: batchUUID=" + batchUUID +
                        " tx=" + to_string(tx) +
                        " txState.oldStateRoot=" + txState.persistence[persistence].oldStateRoot +
                        " currentSubState=" + to_string(txState.persistence[persistence].currentSubState) +
                        " substate.oldStateRoot=" + txState.persistence[persistence].subState[currentSubState].oldStateRoot);
#ifdef LOG_TIME_STATISTICS_STATE_MANAGER
                    timeMetricStorage.add("flush UUID cannot find previous tx sub-state", TimeDiff(t));
                    timeMetricStorage.print("State Manager calls");
#endif
                    Unlock();
                    return ZKR_STATE_MANAGER;
                }
                currentSubState = previousSubState;
            }

            // Delete invalid TX sub-states
            if (db.config.stateManagerPurge)
            {
                for (int64_t i = txState.persistence[persistence].subState.size()-1; i>=0; i--)
                {
                    if (!txState.persistence[persistence].subState[i].bValid)
                    {
                        txState.persistence[persistence].subState.erase(txState.persistence[persistence].subState.begin() + i);
                    }
                }

                // Delete unneeded hashes: delete only hashes written previously to the deletion time

                // For all sub-states
                for (uint64_t ss = 0; ss < txState.persistence[persistence].subState.size(); ss++)
                {
                    // For all keys to delete
                    for (uint64_t k = 0; k < txState.persistence[persistence].subState[ss].dbDelete.size(); k++)
                    {
                        // For all previouse sub-states, previous to the current sub-state
                        for (uint64_t pss = 0; pss < ss; pss++)
                        {
                            txState.persistence[persistence].subState[pss].dbWrite.erase(txState.persistence[persistence].subState[ss].dbDelete[k]);
                        }
                    }
                    txState.persistence[persistence].subState[ss].dbDelete.clear();
                }
            }

            // Save data to database

            // For all sub-states
            for (uint64_t ss = 0; ss < txState.persistence[persistence].subState.size(); ss++)
            {
                // For all keys to write
                unordered_map<string, string>::const_iterator writeIt;
                for ( writeIt = txState.persistence[persistence].subState[ss].dbWrite.begin();
                      writeIt != txState.persistence[persistence].subState[ss].dbWrite.end();
                      writeIt++ )
                {
                    zkr = db.write(writeIt->first, NULL, writeIt->second, persistence == PERSISTENCE_DATABASE ? 1 : 0);
                    if (zkr != ZKR_SUCCESS)
                    {
                        zklog.error("StateManager64::flush() failed calling db.write() result=" + zkresult2string(zkr));
                        state.erase(it);

                        TimerStopAndLog(STATE_MANAGER_FLUSH);

#ifdef LOG_TIME_STATISTICS_STATE_MANAGER
                        timeMetricStorage.add("flush error db.write", TimeDiff(t));
                        timeMetricStorage.print("State Manager calls");
#endif
                        Unlock();
                        return zkr;
                    }
                }
            }

            if (persistence == PERSISTENCE_DATABASE)
            {
                vector<Goldilocks::Element> fea;
                string2fea(db.fr, txState.persistence[persistence].newStateRoot, fea);
                if (fea.size() != 4)
                {
                    zklog.error("StateManager64::flush() failed calling string2fea() fea.size=" + to_string(fea.size()));
                    state.erase(it);

                    TimerStopAndLog(STATE_MANAGER_FLUSH);

#ifdef LOG_TIME_STATISTICS_STATE_MANAGER
                    timeMetricStorage.add("flush error string2fea", TimeDiff(t));
                    timeMetricStorage.print("State Manager calls");
#endif
                    Unlock();
                    return zkr;

                }
                Goldilocks::Element newStateRootFea[4];
                newStateRootFea[0] = fea[3];
                newStateRootFea[1] = fea[2];
                newStateRootFea[2] = fea[1];
                newStateRootFea[3] = fea[0];

                zkr = db.updateStateRoot(newStateRootFea);
                if (zkr != ZKR_SUCCESS)
                {
                    zklog.error("StateManager64::flush() failed calling db.updateStateRoot() result=" + zkresult2string(zkr));
                    state.erase(it);

                    TimerStopAndLog(STATE_MANAGER_FLUSH);

#ifdef LOG_TIME_STATISTICS_STATE_MANAGER
                    timeMetricStorage.add("flush error db.updateStateRoot", TimeDiff(t));
                    timeMetricStorage.print("State Manager calls");
#endif
                    Unlock();
                    return zkr;
                }
            }
        }
    }
    
    // Delete this batch UUID state
    state.erase(it);

    zkr = db.flush(flushId, lastSentFlushId);
    if (zkr != ZKR_SUCCESS)
    {
        zklog.error("StateManager64::flush() failed calling db.flush() result=" + zkresult2string(zkr));
    }

    TimerStopAndLog(STATE_MANAGER_FLUSH);

#ifdef LOG_TIME_STATISTICS_STATE_MANAGER
    timeMetricStorage.add("flush success", TimeDiff(t));
    timeMetricStorage.print("State Manager calls");
#endif

    Unlock();

    return zkr;
}

void StateManager64::print (bool bDbContent)
{
    uint64_t totalDbWrites[PERSISTENCE_SIZE] = {0, 0, 0};
    uint64_t totalDbDeletes[PERSISTENCE_SIZE] = {0, 0, 0};
    zklog.info("StateManager64::print():");
    zklog.info("state.size=" + to_string(state.size()));
    unordered_map<string, BatchState64>::const_iterator stateIt;
    uint64_t batchStateCounter = 0;
    for (stateIt = state.begin(); stateIt != state.end(); stateIt++)
    {
        const BatchState64 &batchState = stateIt->second;
        zklog.info("  batchState=" + to_string(batchStateCounter));
        batchStateCounter++;
        zklog.info("  BatchUUID=" + stateIt->first);
        zklog.info("  oldStateRoot=" + batchState.oldStateRoot);
        zklog.info("  currentStateRoot=" + batchState.currentStateRoot);
        zklog.info("  currentTx=" + to_string(batchState.currentTx));

        for (uint64_t tx=0; tx<batchState.txState.size(); tx++)
        {

            zklog.info("    TX=" + to_string(tx));
            const TxState64 &txState = batchState.txState[tx];

            for (uint64_t persistence = 0; persistence < PERSISTENCE_SIZE; persistence++)
            {
                zklog.info("      persistence=" + to_string(persistence) + "=" + persistence2string((Persistence)persistence));
                zklog.info("        oldStateRoot=" + txState.persistence[persistence].oldStateRoot);
                zklog.info("        newStateRoot=" + txState.persistence[persistence].newStateRoot);
                zklog.info("        currentSubState=" + to_string(txState.persistence[persistence].currentSubState));
                zklog.info("        txSubState.size=" + to_string(txState.persistence[persistence].subState.size()));
                for (uint64_t i=0; i<txState.persistence[persistence].subState.size(); i++)
                {
                    const TxSubState64 &txSubState = txState.persistence[persistence].subState[i];
                    zklog.info("          txSubState=" + to_string(i));
                    zklog.info("            oldStateRoot=" + txSubState.oldStateRoot);
                    zklog.info("            newStateRoot=" + txSubState.newStateRoot);
                    zklog.info("            valid=" + to_string(txSubState.bValid));
                    zklog.info("            previousSubState=" + to_string(txSubState.previousSubState));
                    zklog.info("            dbWrite.size=" + to_string(txSubState.dbWrite.size()));

                    totalDbWrites[persistence] += txSubState.dbWrite.size();
                    if (bDbContent)
                    {
                        unordered_map<string, string>::const_iterator dbIt;
                        for (dbIt = txSubState.dbWrite.begin(); dbIt != txSubState.dbWrite.end(); dbIt++)
                        {
                            zklog.info("              " + dbIt->first);
                        }
                    }
                    zklog.info("            dbDelete.size=" + to_string(txSubState.dbDelete.size()));
                    totalDbDeletes[persistence] += txSubState.dbDelete.size();
                    if (bDbContent)
                    {
                        for (uint64_t j=0; j<txSubState.dbDelete.size(); j++)
                        {
                            zklog.info("              " + txSubState.dbDelete[j]);
                        }
                    }
                }
            }
        }
    }

    uint64_t totalWrites = 0;
    uint64_t totalDeletes = 0;
    for (uint64_t persistence=0; persistence<PERSISTENCE_SIZE; persistence++)
    {
        zklog.info("total db writes[" + persistence2string((Persistence)persistence) + "]=" + to_string(totalDbWrites[persistence]));
        totalWrites += totalDbWrites[persistence];
        zklog.info("total db deletes[" + persistence2string((Persistence)persistence) + "]=" + to_string(totalDbDeletes[persistence]));
        totalDeletes += totalDbDeletes[persistence];
    }
    zklog.info("total writes=" + to_string(totalWrites));
    zklog.info("total deletes=" + to_string(totalDeletes));
}