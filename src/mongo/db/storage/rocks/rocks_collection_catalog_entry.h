// rocks_collection_catalog_entry.h

/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/db/catalog/collection_catalog_entry.h"

namespace rocksdb {
    class DB;
}

namespace mongo {

    class RocksEngine;

    class RocksCollectionCatalogEntry : public CollectionCatalogEntry {
    public:
        RocksCollectionCatalogEntry( RocksEngine* engine, const StringData& ns );

        virtual ~RocksCollectionCatalogEntry(){}

        virtual CollectionOptions getCollectionOptions(OperationContext* txn) const;

        // ------- indexes ----------

        virtual int getTotalIndexCount() const;

        virtual int getCompletedIndexCount() const;

        virtual int getMaxAllowedIndexes() const;

        virtual void getAllIndexes( std::vector<std::string>* names ) const;

        virtual BSONObj getIndexSpec( const StringData& idxName ) const;

        virtual bool isIndexMultikey( const StringData& indexName) const;

        virtual bool setIndexIsMultikey(OperationContext* txn,
                                        const StringData& indexName,
                                        bool multikey = true);

        virtual DiskLoc getIndexHead( const StringData& indexName ) const;

        virtual void setIndexHead( OperationContext* txn,
                                   const StringData& indexName,
                                   const DiskLoc& newHead );

        virtual bool isIndexReady( const StringData& indexName ) const;

        virtual Status removeIndex( OperationContext* txn,
                                    const StringData& indexName );

        virtual Status prepareForIndexBuild( OperationContext* txn,
                                             const IndexDescriptor* spec );

        virtual void indexBuildSuccess( OperationContext* txn,
                                        const StringData& indexName );

        /* Updates the expireAfterSeconds field of the given index to the value in newExpireSecs.
         * The specified index must already contain an expireAfterSeconds field, and the value in
         * that field and newExpireSecs must both be numeric.
         */
        virtual void updateTTLSetting( OperationContext* txn,
                                       const StringData& idxName,
                                       long long newExpireSeconds );

        // ------ internal api

        BSONObj getIndexSpec( const StringData& idxName, rocksdb::DB* db ) const;

        // called once when collection is created.
        void createMetaData();

        // when collection is dropped, call this
        // all indexes have to be dropped first.
        void dropMetaData();

        const string metaDataKey() { return _metaDataKey; }

        struct IndexMetaData {
            IndexMetaData() {}
            IndexMetaData( BSONObj s, bool r, DiskLoc h, bool m )
                : spec( s ), ready( r ), head( h ), multikey( m ) {}

            BSONObj spec;
            bool ready;
            DiskLoc head;
            bool multikey;
        };

        struct MetaData {
            void parse( const BSONObj& obj );
            BSONObj toBSON() const;

            int findIndexOffset( const StringData& name ) const;

            /**
             * Removes information about an index from the MetaData. Returns true if an index
             * called name existed and was deleted, and false otherwise.
             */
            bool eraseIndex( const StringData& name );

            std::string ns;
            std::vector<IndexMetaData> indexes;
        };

    private:
        MetaData _getMetaData() const;
        MetaData _getMetaData( rocksdb::DB* db ) const;

        MetaData _getMetaData_inlock() const;
        MetaData _getMetaData_inlock( rocksdb::DB* db ) const;

        void _putMetaData_inlock( const MetaData& in );

        RocksEngine* _engine; // not owned

        // the name of the column family which holds the metadata.
        const string _metaDataKey;

        // lock which must be acquired before calling _getMetaData_inlock(). Protects the metadata
        // stored in the metadata column family.
        mutable boost::mutex _metaDataMutex;
    };

}
