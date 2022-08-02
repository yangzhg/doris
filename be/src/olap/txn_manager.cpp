// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "txn_manager.h"

#include <rapidjson/document.h>
#include <signal.h>
#include <thrift/protocol/TDebugProtocol.h>

#include <algorithm>
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <cstdio>
#include <filesystem>
#include <new>
#include <queue>
#include <random>
#include <set>

#include "olap/base_compaction.h"
#include "olap/cumulative_compaction.h"
#include "olap/data_dir.h"
#include "olap/delta_writer.h"
#include "olap/lru_cache.h"
#include "olap/push_handler.h"
#include "olap/reader.h"
#include "olap/rowset/rowset_meta_manager.h"
#include "olap/schema_change.h"
#include "olap/storage_engine.h"
#include "olap/tablet_meta.h"
#include "olap/tablet_meta_manager.h"
#include "olap/utils.h"
#include "rowset/beta_rowset.h"
#include "util/doris_metrics.h"
#include "util/pretty_printer.h"
#include "util/time.h"

using apache::thrift::ThriftDebugString;
using std::filesystem::canonical;
using std::filesystem::directory_iterator;
using std::filesystem::path;
using std::filesystem::recursive_directory_iterator;
using std::back_inserter;
using std::copy;
using std::inserter;
using std::list;
using std::map;
using std::nothrow;
using std::pair;
using std::priority_queue;
using std::set;
using std::set_difference;
using std::string;
using std::stringstream;
using std::vector;

namespace doris {

TxnManager::TxnManager(int32_t txn_map_shard_size, int32_t txn_shard_size)
        : _txn_map_shard_size(txn_map_shard_size), _txn_shard_size(txn_shard_size) {
    DCHECK_GT(_txn_map_shard_size, 0);
    DCHECK_GT(_txn_shard_size, 0);
    DCHECK_EQ(_txn_map_shard_size & (_txn_map_shard_size - 1), 0);
    DCHECK_EQ(_txn_shard_size & (_txn_shard_size - 1), 0);
    _txn_map_locks = new std::shared_mutex[_txn_map_shard_size];
    _txn_tablet_maps = new txn_tablet_map_t[_txn_map_shard_size];
    _txn_partition_maps = new txn_partition_map_t[_txn_map_shard_size];
    _txn_mutex = new std::mutex[_txn_shard_size];
    _txn_tablet_delta_writer_map = new txn_tablet_delta_writer_map_t[_txn_map_shard_size];
    _txn_tablet_delta_writer_map_locks = new std::shared_mutex[_txn_map_shard_size];
}

Status TxnManager::prepare_txn(TPartitionId partition_id, const TabletSharedPtr& tablet,
                               TTransactionId transaction_id, const PUniqueId& load_id) {
    return prepare_txn(partition_id, transaction_id, tablet->tablet_id(), tablet->schema_hash(),
                       tablet->tablet_uid(), load_id);
}

Status TxnManager::commit_txn(TPartitionId partition_id, const TabletSharedPtr& tablet,
                              TTransactionId transaction_id, const PUniqueId& load_id,
                              const RowsetSharedPtr& rowset_ptr, bool is_recovery) {
    return commit_txn(tablet->data_dir()->get_meta(), partition_id, transaction_id,
                      tablet->tablet_id(), tablet->schema_hash(), tablet->tablet_uid(), load_id,
                      rowset_ptr, is_recovery);
}

Status TxnManager::publish_txn(TPartitionId partition_id, const TabletSharedPtr& tablet,
                               TTransactionId transaction_id, const Version& version) {
    return publish_txn(tablet->data_dir()->get_meta(), partition_id, transaction_id,
                       tablet->tablet_id(), tablet->schema_hash(), tablet->tablet_uid(), version);
}

// delete the txn from manager if it is not committed(not have a valid rowset)
Status TxnManager::rollback_txn(TPartitionId partition_id, const TabletSharedPtr& tablet,
                                TTransactionId transaction_id) {
    return rollback_txn(partition_id, transaction_id, tablet->tablet_id(), tablet->schema_hash(),
                        tablet->tablet_uid());
}

Status TxnManager::delete_txn(TPartitionId partition_id, const TabletSharedPtr& tablet,
                              TTransactionId transaction_id) {
    return delete_txn(tablet->data_dir()->get_meta(), partition_id, transaction_id,
                      tablet->tablet_id(), tablet->schema_hash(), tablet->tablet_uid());
}

// prepare txn should always be allowed because ingest task will be retried
// could not distinguish rollup, schema change or base table, prepare txn successfully will allow
// ingest retried
Status TxnManager::prepare_txn(TPartitionId partition_id, TTransactionId transaction_id,
                               TTabletId tablet_id, SchemaHash schema_hash, TabletUid tablet_uid,
                               const PUniqueId& load_id) {
    TxnKey key(partition_id, transaction_id);
    TabletInfo tablet_info(tablet_id, schema_hash, tablet_uid);
    std::lock_guard<std::shared_mutex> txn_wrlock(_get_txn_map_lock(transaction_id));
    txn_tablet_map_t& txn_tablet_map = _get_txn_tablet_map(transaction_id);
    auto it = txn_tablet_map.find(key);
    if (it != txn_tablet_map.end()) {
        auto load_itr = it->second.find(tablet_info);
        if (load_itr != it->second.end()) {
            // found load for txn,tablet
            // case 1: user commit rowset, then the load id must be equal
            TabletTxnInfo& load_info = load_itr->second;
            // check if load id is equal
            if (load_info.load_id.hi() == load_id.hi() && load_info.load_id.lo() == load_id.lo() &&
                load_info.rowset != nullptr) {
                LOG(WARNING) << "find transaction exists when add to engine."
                             << "partition_id: " << key.first << ", transaction_id: " << key.second
                             << ", tablet: " << tablet_info.to_string();
                return Status::OK();
            }
        }
    }

    // check if there are too many transactions on running.
    // if yes, reject the request.
    txn_partition_map_t& txn_partition_map = _get_txn_partition_map(transaction_id);
    if (txn_partition_map.size() > config::max_runnings_transactions_per_txn_map) {
        LOG(WARNING) << "too many transactions: " << txn_tablet_map.size()
                     << ", limit: " << config::max_runnings_transactions_per_txn_map;
        return Status::OLAPInternalError(OLAP_ERR_TOO_MANY_TRANSACTIONS);
    }

    // not found load id
    // case 1: user start a new txn, rowset_ptr = null
    // case 2: loading txn from meta env
    TabletTxnInfo load_info(load_id, nullptr);
    txn_tablet_map[key][tablet_info] = load_info;
    _insert_txn_partition_map_unlocked(transaction_id, partition_id);

    VLOG_NOTICE << "add transaction to engine successfully."
                << "partition_id: " << key.first << ", transaction_id: " << key.second
                << ", tablet: " << tablet_info.to_string();
    return Status::OK();
}

Status TxnManager::commit_txn(OlapMeta* meta, TPartitionId partition_id,
                              TTransactionId transaction_id, TTabletId tablet_id,
                              SchemaHash schema_hash, TabletUid tablet_uid,
                              const PUniqueId& load_id, const RowsetSharedPtr& rowset_ptr,
                              bool is_recovery) {
    if (partition_id < 1 || transaction_id < 1 || tablet_id < 1) {
        LOG(FATAL) << "invalid commit req "
                   << " partition_id=" << partition_id << " transaction_id=" << transaction_id
                   << " tablet_id=" << tablet_id;
    }
    pair<int64_t, int64_t> key(partition_id, transaction_id);
    TabletInfo tablet_info(tablet_id, schema_hash, tablet_uid);
    if (rowset_ptr == nullptr) {
        LOG(WARNING) << "could not commit txn because rowset ptr is null. "
                     << "partition_id: " << key.first << ", transaction_id: " << key.second
                     << ", tablet: " << tablet_info.to_string();
        return Status::OLAPInternalError(OLAP_ERR_ROWSET_INVALID);
    }

    std::unique_lock<std::mutex> txn_lock(_get_txn_lock(transaction_id));
    {
        // get tx
        std::shared_lock rdlock(_get_txn_map_lock(transaction_id));
        txn_tablet_map_t& txn_tablet_map = _get_txn_tablet_map(transaction_id);
        auto it = txn_tablet_map.find(key);
        if (it != txn_tablet_map.end()) {
            auto load_itr = it->second.find(tablet_info);
            if (load_itr != it->second.end()) {
                // found load for txn,tablet
                // case 1: user commit rowset, then the load id must be equal
                TabletTxnInfo& load_info = load_itr->second;
                // check if load id is equal
                if (load_info.load_id.hi() == load_id.hi() &&
                    load_info.load_id.lo() == load_id.lo() && load_info.rowset != nullptr &&
                    load_info.rowset->rowset_id() == rowset_ptr->rowset_id()) {
                    // find a rowset with same rowset id, then it means a duplicate call
                    LOG(INFO) << "find rowset exists when commit transaction to engine."
                              << "partition_id: " << key.first << ", transaction_id: " << key.second
                              << ", tablet: " << tablet_info.to_string()
                              << ", rowset_id: " << load_info.rowset->rowset_id();
                    return Status::OK();
                } else if (load_info.load_id.hi() == load_id.hi() &&
                           load_info.load_id.lo() == load_id.lo() && load_info.rowset != nullptr &&
                           load_info.rowset->rowset_id() != rowset_ptr->rowset_id()) {
                    // find a rowset with different rowset id, then it should not happen, just return errors
                    LOG(WARNING) << "find rowset exists when commit transaction to engine. but "
                                    "rowset ids "
                                    "are not same."
                                 << "partition_id: " << key.first
                                 << ", transaction_id: " << key.second
                                 << ", tablet: " << tablet_info.to_string()
                                 << ", exist rowset_id: " << load_info.rowset->rowset_id()
                                 << ", new rowset_id: " << rowset_ptr->rowset_id();
                    return Status::OLAPInternalError(OLAP_ERR_PUSH_TRANSACTION_ALREADY_EXIST);
                }
            }
        }
    }

    // if not in recovery mode, then should persist the meta to meta env
    // save meta need access disk, it maybe very slow, so that it is not in global txn lock
    // it is under a single txn lock
    if (!is_recovery) {
        Status save_status = RowsetMetaManager::save(meta, tablet_uid, rowset_ptr->rowset_id(),
                                                     rowset_ptr->rowset_meta()->get_rowset_pb());
        if (save_status != Status::OK()) {
            LOG(WARNING) << "save committed rowset failed. when commit txn rowset_id:"
                         << rowset_ptr->rowset_id() << "tablet id: " << tablet_id
                         << "txn id:" << transaction_id;
            return Status::OLAPInternalError(OLAP_ERR_ROWSET_SAVE_FAILED);
        }
    }

    {
        std::lock_guard<std::shared_mutex> wrlock(_get_txn_map_lock(transaction_id));
        TabletTxnInfo load_info(load_id, rowset_ptr);
        txn_tablet_map_t& txn_tablet_map = _get_txn_tablet_map(transaction_id);
        txn_tablet_map[key][tablet_info] = load_info;
        _insert_txn_partition_map_unlocked(transaction_id, partition_id);
        VLOG_NOTICE << "commit transaction to engine successfully."
                    << " partition_id: " << key.first << ", transaction_id: " << key.second
                    << ", tablet: " << tablet_info.to_string()
                    << ", rowsetid: " << rowset_ptr->rowset_id()
                    << ", version: " << rowset_ptr->version().first;
    }
    return Status::OK();
}

// remove a txn from txn manager
Status TxnManager::publish_txn(OlapMeta* meta, TPartitionId partition_id,
                               TTransactionId transaction_id, TTabletId tablet_id,
                               SchemaHash schema_hash, TabletUid tablet_uid,
                               const Version& version) {
    pair<int64_t, int64_t> key(partition_id, transaction_id);
    TabletInfo tablet_info(tablet_id, schema_hash, tablet_uid);
    RowsetSharedPtr rowset_ptr = nullptr;
    std::unique_lock<std::mutex> txn_lock(_get_txn_lock(transaction_id));
    {
        std::shared_lock rlock(_get_txn_map_lock(transaction_id));
        txn_tablet_map_t& txn_tablet_map = _get_txn_tablet_map(transaction_id);
        auto it = txn_tablet_map.find(key);
        if (it != txn_tablet_map.end()) {
            auto load_itr = it->second.find(tablet_info);
            if (load_itr != it->second.end()) {
                // found load for txn,tablet
                // case 1: user commit rowset, then the load id must be equal
                TabletTxnInfo& load_info = load_itr->second;
                rowset_ptr = load_info.rowset;
            }
        }
    }
    // save meta need access disk, it maybe very slow, so that it is not in global txn lock
    // it is under a single txn lock
    if (rowset_ptr != nullptr) {
        // TODO(ygl): rowset is already set version here, memory is changed, if save failed
        // it maybe a fatal error
        rowset_ptr->make_visible(version);
        Status save_status = RowsetMetaManager::save(meta, tablet_uid, rowset_ptr->rowset_id(),
                                                     rowset_ptr->rowset_meta()->get_rowset_pb());
        if (save_status != Status::OK()) {
            LOG(WARNING) << "save committed rowset failed. when publish txn rowset_id:"
                         << rowset_ptr->rowset_id() << ", tablet id: " << tablet_id
                         << ", txn id:" << transaction_id;
            return Status::OLAPInternalError(OLAP_ERR_ROWSET_SAVE_FAILED);
        }
    } else {
        return Status::OLAPInternalError(OLAP_ERR_TRANSACTION_NOT_EXIST);
    }
    {
        std::lock_guard<std::shared_mutex> wrlock(_get_txn_map_lock(transaction_id));
        txn_tablet_map_t& txn_tablet_map = _get_txn_tablet_map(transaction_id);
        auto it = txn_tablet_map.find(key);
        if (it != txn_tablet_map.end()) {
            it->second.erase(tablet_info);
            VLOG_NOTICE << "publish txn successfully."
                        << " partition_id: " << key.first << ", txn_id: " << key.second
                        << ", tablet: " << tablet_info.to_string()
                        << ", rowsetid: " << rowset_ptr->rowset_id()
                        << ", version: " << version.first << "," << version.second;
            if (it->second.empty()) {
                txn_tablet_map.erase(it);
                _clear_txn_partition_map_unlocked(transaction_id, partition_id);
            }
        }
    }
    auto tablet = StorageEngine::instance()->tablet_manager()->get_tablet(tablet_id);
#ifdef BE_TEST
    if (tablet == nullptr) {
        return Status::OK();
    }
#endif
    // Check if have to build extra delete bitmap for table of UNIQUE_KEY model
    if (!tablet->enable_unique_key_merge_on_write() ||
        tablet->tablet_meta()->preferred_rowset_type() != RowsetTypePB::BETA_ROWSET ||
        rowset_ptr->keys_type() != KeysType::UNIQUE_KEYS) {
        return Status::OK();
    }
    CHECK(version.first == version.second) << "impossible: " << version;

    // For each key in current set, check if it overwrites any previously
    // written keys
    OlapStopWatch watch;
    std::vector<segment_v2::SegmentSharedPtr> segments;
    std::vector<segment_v2::SegmentSharedPtr> pre_segments;
    auto beta_rowset = reinterpret_cast<BetaRowset*>(rowset_ptr.get());
    Status st = beta_rowset->load_segments(&segments);
    if (!st.ok()) return st;
    // lock tablet meta to modify delete bitmap
    std::lock_guard<std::shared_mutex> meta_wrlock(tablet->get_header_lock());
    for (auto& seg : segments) {
        seg->load_index(); // We need index blocks to iterate
        auto pk_idx = seg->get_primary_key_index();
        int cnt = 0;
        int total = pk_idx->num_rows();
        int32_t remaining = total;
        bool exact_match = false;
        std::string last_key;
        int batch_size = 1024;
        MemPool pool;
        while (remaining > 0) {
            std::unique_ptr<segment_v2::IndexedColumnIterator> iter;
            RETURN_IF_ERROR(pk_idx->new_iterator(&iter));

            size_t num_to_read = std::min(batch_size, remaining);
            std::unique_ptr<ColumnVectorBatch> cvb;
            RETURN_IF_ERROR(ColumnVectorBatch::create(num_to_read, false, pk_idx->type_info(),
                                                      nullptr, &cvb));
            ColumnBlock block(cvb.get(), &pool);
            ColumnBlockView column_block_view(&block);
            Slice last_key_slice(last_key);
            RETURN_IF_ERROR(iter->seek_at_or_after(&last_key_slice, &exact_match));

            size_t num_read = num_to_read;
            RETURN_IF_ERROR(iter->next_batch(&num_read, &column_block_view));
            DCHECK(num_to_read == num_read);
            last_key = (reinterpret_cast<const Slice*>(cvb->cell_ptr(num_read - 1)))->to_string();

            // exclude last_key, last_key will be read in next batch.
            if (num_read == batch_size && num_read != remaining) {
                num_read -= 1;
            }
            for (size_t i = 0; i < num_read; i++) {
                const Slice* key = reinterpret_cast<const Slice*>(cvb->cell_ptr(i));
                // first check if exist in pre segment
                bool find = _check_pk_in_pre_segments(pre_segments, *key, tablet, version);
                if (find) {
                    cnt++;
                    continue;
                }
                RowLocation loc;
                st = tablet->lookup_row_key(*key, &loc, version.first - 1);
                CHECK(st.ok() || st.is_not_found());
                if (st.is_not_found()) continue;
                ++cnt;
                // TODO: we can just set a bitmap onece we are done while iteration
                tablet->tablet_meta()->delete_bitmap().add(
                        {loc.rowset_id, loc.segment_id, version.first}, loc.row_id);
            }
            remaining -= num_read;
        }

        LOG(INFO) << "construct delete bitmap tablet: " << tablet->tablet_id()
                  << " rowset: " << beta_rowset->rowset_id() << " segment: " << seg->id()
                  << " version: " << version << " delete: " << cnt << "/" << total;
        pre_segments.emplace_back(seg);
    }
    tablet->save_meta();
    LOG(INFO) << "finished to update delete bitmap, tablet: " << tablet->tablet_id()
              << " version: " << version << ", elapse(us): " << watch.get_elapse_time_us();
    return Status::OK();
}

bool TxnManager::_check_pk_in_pre_segments(
        const std::vector<segment_v2::SegmentSharedPtr>& pre_segments, const Slice& key,
        TabletSharedPtr tablet, const Version& version) {
    for (auto it = pre_segments.rbegin(); it != pre_segments.rend(); ++it) {
        RowLocation loc;
        auto st = (*it)->lookup_row_key(key, &loc);
        CHECK(st.ok() || st.is_not_found());
        if (st.is_not_found()) {
            continue;
        }
        tablet->tablet_meta()->delete_bitmap().add({loc.rowset_id, loc.segment_id, version.first},
                                                   loc.row_id);
        return true;
    }
    return false;
}

// txn could be rollbacked if it does not have related rowset
// if the txn has related rowset then could not rollback it, because it
// may be committed in another thread and our current thread meets errors when writing to data file
// BE has to wait for fe call clear txn api
Status TxnManager::rollback_txn(TPartitionId partition_id, TTransactionId transaction_id,
                                TTabletId tablet_id, SchemaHash schema_hash, TabletUid tablet_uid) {
    pair<int64_t, int64_t> key(partition_id, transaction_id);
    TabletInfo tablet_info(tablet_id, schema_hash, tablet_uid);
    std::lock_guard<std::shared_mutex> wrlock(_get_txn_map_lock(transaction_id));
    txn_tablet_map_t& txn_tablet_map = _get_txn_tablet_map(transaction_id);
    auto it = txn_tablet_map.find(key);
    if (it != txn_tablet_map.end()) {
        auto load_itr = it->second.find(tablet_info);
        if (load_itr != it->second.end()) {
            // found load for txn,tablet
            // case 1: user commit rowset, then the load id must be equal
            TabletTxnInfo& load_info = load_itr->second;
            if (load_info.rowset != nullptr) {
                // if rowset is not null, it means other thread may commit the rowset
                // should not delete txn any more
                return Status::OLAPInternalError(OLAP_ERR_TRANSACTION_ALREADY_COMMITTED);
            }
        }
        it->second.erase(tablet_info);
        LOG(INFO) << "rollback transaction from engine successfully."
                  << " partition_id: " << key.first << ", transaction_id: " << key.second
                  << ", tablet: " << tablet_info.to_string();
        if (it->second.empty()) {
            txn_tablet_map.erase(it);
            _clear_txn_partition_map_unlocked(transaction_id, partition_id);
        }
    }
    return Status::OK();
}

// fe call this api to clear unused rowsets in be
// could not delete the rowset if it already has a valid version
Status TxnManager::delete_txn(OlapMeta* meta, TPartitionId partition_id,
                              TTransactionId transaction_id, TTabletId tablet_id,
                              SchemaHash schema_hash, TabletUid tablet_uid) {
    pair<int64_t, int64_t> key(partition_id, transaction_id);
    TabletInfo tablet_info(tablet_id, schema_hash, tablet_uid);
    std::lock_guard<std::shared_mutex> txn_wrlock(_get_txn_map_lock(transaction_id));
    txn_tablet_map_t& txn_tablet_map = _get_txn_tablet_map(transaction_id);
    auto it = txn_tablet_map.find(key);
    if (it == txn_tablet_map.end()) {
        return Status::OLAPInternalError(OLAP_ERR_TRANSACTION_NOT_EXIST);
    }
    auto load_itr = it->second.find(tablet_info);
    if (load_itr != it->second.end()) {
        // found load for txn,tablet
        // case 1: user commit rowset, then the load id must be equal
        TabletTxnInfo& load_info = load_itr->second;
        if (load_info.rowset != nullptr && meta != nullptr) {
            if (load_info.rowset->version().first > 0) {
                LOG(WARNING) << "could not delete transaction from engine, "
                             << "just remove it from memory not delete from disk"
                             << " because related rowset already published."
                             << ",partition_id: " << key.first << ", transaction_id: " << key.second
                             << ", tablet: " << tablet_info.to_string()
                             << ", rowset id: " << load_info.rowset->rowset_id()
                             << ", version: " << load_info.rowset->version().first;
                return Status::OLAPInternalError(OLAP_ERR_TRANSACTION_ALREADY_COMMITTED);
            } else {
                RowsetMetaManager::remove(meta, tablet_uid, load_info.rowset->rowset_id());
#ifndef BE_TEST
                StorageEngine::instance()->add_unused_rowset(load_info.rowset);
#endif
                VLOG_NOTICE << "delete transaction from engine successfully."
                            << " partition_id: " << key.first << ", transaction_id: " << key.second
                            << ", tablet: " << tablet_info.to_string() << ", rowset: "
                            << (load_info.rowset != nullptr
                                        ? load_info.rowset->rowset_id().to_string()
                                        : "0");
            }
        }
    }
    it->second.erase(tablet_info);
    if (it->second.empty()) {
        txn_tablet_map.erase(it);
        _clear_txn_partition_map_unlocked(transaction_id, partition_id);
    }
    return Status::OK();
}

void TxnManager::get_tablet_related_txns(TTabletId tablet_id, SchemaHash schema_hash,
                                         TabletUid tablet_uid, int64_t* partition_id,
                                         std::set<int64_t>* transaction_ids) {
    if (partition_id == nullptr || transaction_ids == nullptr) {
        LOG(WARNING) << "parameter is null when get transactions by tablet";
        return;
    }

    TabletInfo tablet_info(tablet_id, schema_hash, tablet_uid);
    for (int32_t i = 0; i < _txn_map_shard_size; i++) {
        std::shared_lock txn_rdlock(_txn_map_locks[i]);
        txn_tablet_map_t& txn_tablet_map = _txn_tablet_maps[i];
        for (auto& it : txn_tablet_map) {
            if (it.second.find(tablet_info) != it.second.end()) {
                *partition_id = it.first.first;
                transaction_ids->insert(it.first.second);
                VLOG_NOTICE << "find transaction on tablet."
                            << "partition_id: " << it.first.first
                            << ", transaction_id: " << it.first.second
                            << ", tablet: " << tablet_info.to_string();
            }
        }
    }
}

// force drop all txns related with the tablet
// maybe lock error, because not get txn lock before remove from meta
void TxnManager::force_rollback_tablet_related_txns(OlapMeta* meta, TTabletId tablet_id,
                                                    SchemaHash schema_hash, TabletUid tablet_uid) {
    TabletInfo tablet_info(tablet_id, schema_hash, tablet_uid);
    for (int32_t i = 0; i < _txn_map_shard_size; i++) {
        std::lock_guard<std::shared_mutex> txn_wrlock(_txn_map_locks[i]);
        txn_tablet_map_t& txn_tablet_map = _txn_tablet_maps[i];
        for (auto it = txn_tablet_map.begin(); it != txn_tablet_map.end();) {
            auto load_itr = it->second.find(tablet_info);
            if (load_itr != it->second.end()) {
                TabletTxnInfo& load_info = load_itr->second;
                if (load_info.rowset != nullptr && meta != nullptr) {
                    LOG(INFO) << " delete transaction from engine "
                              << ", tablet: " << tablet_info.to_string()
                              << ", rowset id: " << load_info.rowset->rowset_id();
                    RowsetMetaManager::remove(meta, tablet_uid, load_info.rowset->rowset_id());
                }
                LOG(INFO) << "remove tablet related txn."
                          << " partition_id: " << it->first.first
                          << ", transaction_id: " << it->first.second
                          << ", tablet: " << tablet_info.to_string() << ", rowset: "
                          << (load_info.rowset != nullptr
                                      ? load_info.rowset->rowset_id().to_string()
                                      : "0");
                it->second.erase(tablet_info);
            }
            if (it->second.empty()) {
                _clear_txn_partition_map_unlocked(it->first.second, it->first.first);
                it = txn_tablet_map.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void TxnManager::get_txn_related_tablets(const TTransactionId transaction_id,
                                         TPartitionId partition_id,
                                         std::map<TabletInfo, RowsetSharedPtr>* tablet_infos) {
    // get tablets in this transaction
    pair<int64_t, int64_t> key(partition_id, transaction_id);
    std::shared_lock txn_rdlock(_get_txn_map_lock(transaction_id));
    txn_tablet_map_t& txn_tablet_map = _get_txn_tablet_map(transaction_id);
    auto it = txn_tablet_map.find(key);
    if (it == txn_tablet_map.end()) {
        VLOG_NOTICE << "could not find tablet for"
                    << " partition_id=" << partition_id << ", transaction_id=" << transaction_id;
        return;
    }
    std::map<TabletInfo, TabletTxnInfo>& load_info_map = it->second;

    // each tablet
    for (auto& load_info : load_info_map) {
        const TabletInfo& tablet_info = load_info.first;
        // must not check rowset == null here, because if rowset == null
        // publish version should failed
        tablet_infos->emplace(tablet_info, load_info.second.rowset);
    }
}

void TxnManager::get_all_related_tablets(std::set<TabletInfo>* tablet_infos) {
    for (int32_t i = 0; i < _txn_map_shard_size; i++) {
        std::shared_lock txn_rdlock(_txn_map_locks[i]);
        for (auto& it : _txn_tablet_maps[i]) {
            for (auto& tablet_load_it : it.second) {
                tablet_infos->emplace(tablet_load_it.first);
            }
        }
    }
}

bool TxnManager::has_txn(TPartitionId partition_id, TTransactionId transaction_id,
                         TTabletId tablet_id, SchemaHash schema_hash, TabletUid tablet_uid) {
    pair<int64_t, int64_t> key(partition_id, transaction_id);
    TabletInfo tablet_info(tablet_id, schema_hash, tablet_uid);
    std::shared_lock txn_rdlock(_get_txn_map_lock(transaction_id));
    txn_tablet_map_t& txn_tablet_map = _get_txn_tablet_map(transaction_id);
    auto it = txn_tablet_map.find(key);
    bool found = it != txn_tablet_map.end() && it->second.find(tablet_info) != it->second.end();

    return found;
}

void TxnManager::build_expire_txn_map(std::map<TabletInfo, std::vector<int64_t>>* expire_txn_map) {
    int64_t now = UnixSeconds();
    // traverse the txn map, and get all expired txns
    for (int32_t i = 0; i < _txn_map_shard_size; i++) {
        std::shared_lock txn_rdlock(_txn_map_locks[i]);
        for (auto& it : _txn_tablet_maps[i]) {
            auto txn_id = it.first.second;
            for (auto& t_map : it.second) {
                double diff = difftime(now, t_map.second.creation_time);
                if (diff >= config::pending_data_expire_time_sec) {
                    (*expire_txn_map)[t_map.first].push_back(txn_id);
                    if (VLOG_IS_ON(3)) {
                        VLOG_NOTICE << "find expired txn."
                                    << " tablet=" << t_map.first.to_string()
                                    << " transaction_id=" << txn_id << " exist_sec=" << diff;
                    }
                }
            }
        }
    }
}

void TxnManager::get_partition_ids(const TTransactionId transaction_id,
                                   std::vector<TPartitionId>* partition_ids) {
    std::shared_lock txn_rdlock(_get_txn_map_lock(transaction_id));
    txn_partition_map_t& txn_partition_map = _get_txn_partition_map(transaction_id);
    auto it = txn_partition_map.find(transaction_id);
    if (it != txn_partition_map.end()) {
        for (int64_t partition_id : it->second) {
            partition_ids->push_back(partition_id);
        }
    }
}

void TxnManager::_insert_txn_partition_map_unlocked(int64_t transaction_id, int64_t partition_id) {
    txn_partition_map_t& txn_partition_map = _get_txn_partition_map(transaction_id);
    auto find = txn_partition_map.find(transaction_id);
    if (find == txn_partition_map.end()) {
        txn_partition_map[transaction_id] = std::unordered_set<int64_t>();
    }
    txn_partition_map[transaction_id].insert(partition_id);
}

void TxnManager::_clear_txn_partition_map_unlocked(int64_t transaction_id, int64_t partition_id) {
    txn_partition_map_t& txn_partition_map = _get_txn_partition_map(transaction_id);
    auto it = txn_partition_map.find(transaction_id);
    if (it != txn_partition_map.end()) {
        it->second.erase(partition_id);
        if (it->second.empty()) {
            txn_partition_map.erase(it);
        }
    }
}

void TxnManager::add_txn_tablet_delta_writer(int64_t transaction_id, int64_t tablet_id,
                                             DeltaWriter* delta_writer) {
    std::lock_guard<std::shared_mutex> txn_wrlock(
            _get_txn_tablet_delta_writer_map_lock(transaction_id));
    txn_tablet_delta_writer_map_t& txn_tablet_delta_writer_map =
            _get_txn_tablet_delta_writer_map(transaction_id);
    auto find = txn_tablet_delta_writer_map.find(transaction_id);
    if (find == txn_tablet_delta_writer_map.end()) {
        txn_tablet_delta_writer_map[transaction_id] = std::map<int64_t, DeltaWriter*>();
    }
    txn_tablet_delta_writer_map[transaction_id][tablet_id] = delta_writer;
}

void TxnManager::finish_slave_tablet_pull_rowset(int64_t transaction_id, int64_t tablet_id,
                                                 int64_t node_id, bool is_succeed) {
    std::lock_guard<std::shared_mutex> txn_wrlock(
            _get_txn_tablet_delta_writer_map_lock(transaction_id));
    txn_tablet_delta_writer_map_t& txn_tablet_delta_writer_map =
            _get_txn_tablet_delta_writer_map(transaction_id);
    auto find_txn = txn_tablet_delta_writer_map.find(transaction_id);
    if (find_txn == txn_tablet_delta_writer_map.end()) {
        LOG(WARNING) << "delta writer manager is not exist, txn_id=" << transaction_id
                     << ", tablet_id=" << tablet_id;
        return;
    }
    auto find_tablet = txn_tablet_delta_writer_map[transaction_id].find(tablet_id);
    if (find_tablet == txn_tablet_delta_writer_map[transaction_id].end()) {
        LOG(WARNING) << "delta writer is not exist, txn_id=" << transaction_id
                     << ", tablet_id=" << tablet_id;
        return;
    }
    DeltaWriter* delta_writer = txn_tablet_delta_writer_map[transaction_id][tablet_id];
    delta_writer->finish_slave_tablet_pull_rowset(node_id, is_succeed);
}

void TxnManager::clear_txn_tablet_delta_writer(int64_t transaction_id) {
    std::lock_guard<std::shared_mutex> txn_wrlock(
            _get_txn_tablet_delta_writer_map_lock(transaction_id));
    txn_tablet_delta_writer_map_t& txn_tablet_delta_writer_map =
            _get_txn_tablet_delta_writer_map(transaction_id);
    auto it = txn_tablet_delta_writer_map.find(transaction_id);
    if (it != txn_tablet_delta_writer_map.end()) {
        txn_tablet_delta_writer_map.erase(it);
    }
    VLOG_CRITICAL << "remove delta writer manager, txn_id=" << transaction_id;
}

} // namespace doris
