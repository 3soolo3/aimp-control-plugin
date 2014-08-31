// Copyright (c) 2014, Alexey Ivanov

#include "stdafx.h"
#include "manager3.6.h"
#include "plugin/logger.h"
#include "utils/sqlite_util.h"
#include "sqlite/sqlite.h"
#include "utils/iunknown_impl.h"
#include "utils/string_encoding.h"
#include "aimp3.60_sdk/Helpers/support.h"
#include "aimp3.60_sdk/Helpers/AIMPString.h"
#include "manager_impl_common.h"
#include <boost/algorithm/string.hpp>
namespace {
using namespace ControlPlugin::PluginLogger;
ModuleLoggerType& logger()
    { return getLogManager().getModuleLogger<AIMPPlayer::AIMPManager>(); }
}

namespace AIMPPlayer
{

using namespace Utilities;
using namespace AIMP36SDK;

template<>
PlaylistID cast(IAIMPPlaylist* playlist)
{
    return reinterpret_cast<PlaylistID>(playlist);
}

template<>
IAIMPPlaylist* cast(PlaylistID id)
{
    return reinterpret_cast<IAIMPPlaylist*>(id);
}

PlaylistEntryID castToPlaylistEntryID (IAIMPPlaylistItem* item)
{
    return reinterpret_cast<PlaylistEntryID>(item);
}

IAIMPPlaylistItem* castToPlaylistItem(PlaylistEntryID id)
{
    return reinterpret_cast<IAIMPPlaylistItem*>(id);
}

class AIMPExtensionPlaylistManagerListener : public AIMP36SDK::IUnknownInterfaceImpl<AIMP36SDK::IAIMPExtensionPlaylistManagerListener>
{
public:
    explicit AIMPExtensionPlaylistManagerListener(AIMPManager36* aimp36_manager)
        : 
        aimp36_manager_(aimp36_manager)
    {}

    virtual void WINAPI PlaylistActivated(IAIMPPlaylist* playlist) {
        aimp36_manager_->playlistActivated(playlist);
    }
	virtual void WINAPI PlaylistAdded(IAIMPPlaylist* playlist) {
        aimp36_manager_->playlistAdded(playlist);
    }
	virtual void WINAPI PlaylistRemoved(IAIMPPlaylist* playlist) {
        aimp36_manager_->playlistRemoved(playlist);
    }

    virtual HRESULT WINAPI QueryInterface(REFIID riid, LPVOID* ppvObj) {
        if (!ppvObj) {
            return E_POINTER;
        }

        if (IID_IUnknown == riid) {
            *ppvObj = this;
            AddRef();
            return S_OK;
        } else if (AIMP36SDK::IID_IAIMPExtensionPlaylistManagerListener == riid) {
            *ppvObj = this;
            AddRef();
            return S_OK;                
        }

        return E_NOINTERFACE;
    }

private:

    AIMPManager36* aimp36_manager_;
};

class AIMPMessageHook : public AIMP36SDK::IUnknownInterfaceImpl<IAIMPMessageHook>
{
public:
    explicit AIMPMessageHook(AIMPManager36* aimp36_manager)
        : 
        aimp36_manager_(aimp36_manager)
    {}

    virtual void WINAPI CoreMessage(DWORD AMessage, int AParam1, void *AParam2, HRESULT *AResult) {
        aimp36_manager_->onAimpCoreMessage(AMessage, AParam1, AParam2, AResult);
    }

private:

    AIMPManager36* aimp36_manager_;
};

AIMPManager36::AIMPManager36(boost::intrusive_ptr<AIMP36SDK::IAIMPCore> aimp36_core)
    :   playlists_db_(nullptr),
        aimp36_core_(aimp36_core)
{
    try {
        initializeAIMPObjects();

        initPlaylistDB();

        // register listeners here
        HRESULT r = aimp36_core->RegisterExtension(IID_IAIMPServicePlaylistManager, new AIMPExtensionPlaylistManagerListener(this));
        if (S_OK != r) {
            throw std::runtime_error("RegisterExtension(IID_IAIMPServicePlaylistManager) failed"); 
        }
    } catch (std::runtime_error& e) {
        throw std::runtime_error( std::string("Error occured during AIMPManager36 initialization. Reason:") + e.what() );
    }
}

AIMPManager36::~AIMPManager36()
{
    // It seems listeners registered by RegisterExtension will be released by AIMP before Finalize call.
    
    aimp_service_message_dispatcher_.reset();
    aimp_service_player_.reset();
    aimp_playlist_queue_.reset();
    aimp_service_playlist_manager_.reset();
    aimp36_core_.reset();

    shutdownPlaylistDB();
}

void AIMPManager36::initializeAIMPObjects()
{
    IAIMPServicePlaylistManager* playlist_manager;
    if (S_OK != aimp36_core_->QueryInterface(IID_IAIMPServicePlaylistManager,
                                             reinterpret_cast<void**>(&playlist_manager)
                                             ) 
        )
    {
        throw std::runtime_error("Creation object IAIMPServicePlaylistManager failed"); 
    }
    aimp_service_playlist_manager_.reset(playlist_manager);
    playlist_manager->Release();

    IAIMPPlaylistQueue* aimp_playlist_queue;
    if (S_OK != aimp_service_playlist_manager_->QueryInterface(IID_IAIMPPlaylistQueue,
                                                               reinterpret_cast<void**>(&aimp_playlist_queue)
                                                               ) 
        )
    {
        throw std::runtime_error("Creation object IAIMPPlaylistQueue failed"); 
    }
    aimp_playlist_queue_.reset(aimp_playlist_queue);
    aimp_playlist_queue->Release();

    IAIMPServicePlayer* aimp_service_player;
    if (S_OK != aimp36_core_->QueryInterface(IID_IAIMPServicePlayer,
                                             reinterpret_cast<void**>(&aimp_service_player)
                                             ) 
        )
    {
        throw std::runtime_error("Creation object IAIMPServicePlayer failed"); 
    }
    aimp_service_player_.reset(aimp_service_player);
    aimp_service_player->Release();

    IAIMPServiceMessageDispatcher* aimp_service_message_dispatcher;
    if (S_OK != aimp36_core_->QueryInterface(IID_IAIMPServiceMessageDispatcher,
                                             reinterpret_cast<void**>(&aimp_service_message_dispatcher)
                                             ) 
        )
    {
        throw std::runtime_error("Creation object IAIMPServiceMessageDispatcher failed"); 
    }
    aimp_service_message_dispatcher_.reset(aimp_service_message_dispatcher);
    aimp_service_message_dispatcher->Release();

    /*
    IAIMPAddonsCoverArtManager* coverart_manager;
    if (S_OK != aimp_service_message_dispatcher_->QueryInterface(IID_IAIMPAddonsCoverArtManager, 
                                                 reinterpret_cast<void**>(&coverart_manager)
                                                 ) 
        )
    {
        throw std::runtime_error("Creation object IAIMPAddonsCoverArtManager failed"); 
    }
    aimp3_coverart_manager_.reset(coverart_manager);
    coverart_manager->Release();
    */
}

void AIMPManager36::initPlaylistDB()
{
#define THROW_IF_NOT_OK_WITH_MSG(rc, msg_expr)  if (SQLITE_OK != rc) { \
                                                    const std::string msg = msg_expr; \
                                                    shutdownPlaylistDB(); \
                                                    throw std::runtime_error(msg); \
                                                }

    int rc = sqlite3_open(":memory:", &playlists_db_);
    THROW_IF_NOT_OK_WITH_MSG( rc, MakeString() << "Playlist database creation failure. Reason: sqlite3_open error "
                                               << rc << ": " << sqlite3_errmsg(playlists_db_) );

    { // add case-insensitivity support to LIKE operator (used for search tracks) since default LIKE operator since it supports case-insensitive search only on ASCII chars by default).
    rc = sqlite3_unicode_init(playlists_db_);
    THROW_IF_NOT_OK_WITH_MSG( rc, MakeString() << "unicode support enabling failure. Reason: sqlite3_unicode_init() error "
                                               << rc << ": " << sqlite3_errmsg(playlists_db_) );
    }

    { // create table for content of all playlists.
    char* errmsg = nullptr;
    ON_BLOCK_EXIT(&sqlite3_free, errmsg);
    rc = sqlite3_exec(playlists_db_,
                      "CREATE TABLE PlaylistsEntries ( playlist_id    INTEGER,"
                                                      "entry_id       INTEGER,"
                                                      "entry_index    INTEGER,"
                                                      "album          VARCHAR(128),"
                                                      "artist         VARCHAR(128),"
                                                      "date           VARCHAR(16),"
                                                      "filename       VARCHAR(260),"
                                                      "genre          VARCHAR(32),"
                                                      "title          VARCHAR(260),"
                                                      "bitrate        INTEGER,"
                                                      "channels_count INTEGER,"
                                                      "duration       INTEGER,"
                                                      "filesize       BIGINT,"
                                                      "rating         TINYINT,"
                                                      "samplerate     INTEGER,"
                                                      "crc32          BIGINT," // use BIGINT since crc32 is uint32.
                                                      "PRIMARY KEY (entry_id)"
                                                      ")",
                      nullptr, /* Callback function */
                      nullptr, /* 1st argument to callback */
                      &errmsg
                      );
    THROW_IF_NOT_OK_WITH_MSG( rc, MakeString() << "Playlist content table creation failure. Reason: sqlite3_exec(create table) error "
                                               << rc << ": " << errmsg );
    }

    { // create table for playlist.
    char* errmsg = nullptr;
    ON_BLOCK_EXIT(&sqlite3_free, errmsg);
    rc = sqlite3_exec(playlists_db_,
                      "CREATE TABLE Playlists ( id              INTEGER,"
                                               "playlist_index  INTEGER,"
                                               "title           VARCHAR(260),"
                                               "entries_count   INTEGER,"
                                               "duration        BIGINT,"
                                               "size_of_entries BIGINT,"
                                               "crc32           BIGINT," // use BIGINT since crc32 is uint32.
                                               "PRIMARY KEY (id)"
                                               ")",
                      nullptr, /* Callback function */
                      nullptr, /* 1st argument to callback */
                      &errmsg
                      );
    THROW_IF_NOT_OK_WITH_MSG( rc, MakeString() << "Playlist table creation failure. Reason: sqlite3_exec(create table) error "
                                               << rc << ": " << errmsg );
    }
#undef THROW_IF_NOT_OK_WITH_MSG
}

void AIMPManager36::shutdownPlaylistDB()
{
    const int rc = sqlite3_close(playlists_db_);
    if (SQLITE_OK != rc) {
        BOOST_LOG_SEV(logger(), error) << "sqlite3_close error: " << rc;
    }
    playlists_db_ = nullptr;
}

void AIMPManager36::playlistActivated(AIMP36SDK::IAIMPPlaylist* /*playlist*/)
{
    // do nothing, but if code will be added, it must not throw any exceptions, since this method called by AIMP.
}

void AIMPManager36::playlistAdded(IAIMPPlaylist* playlist)
{
    try {
        BOOST_LOG_SEV(logger(), debug) << "playlistAdded: id = " << cast<PlaylistID>(playlist);
        playlist_helpers_.emplace_back(playlist, this); // addref playlist to prevent pointer change in playlistRemoved. Also helper will autosubscribe for playlist updates.

        int playlist_index = getPlaylistIndexByHandle(playlist);
        loadPlaylist(playlist, playlist_index);
        loadEntries(playlist);
        notifyAllExternalListeners(EVENT_PLAYLISTS_CONTENT_CHANGE);
    } catch (std::exception& e) {
        BOOST_LOG_SEV(logger(), error) << "Error in "__FUNCTION__ << " for playlist with handle " << cast<PlaylistID>(playlist) << ". Reason: " << e.what();
    } catch (...) {
        // we can't propagate exception from here since it is called from AIMP. Just log unknown error.
        BOOST_LOG_SEV(logger(), error) << "Unknown exception in "__FUNCTION__ << " for playlist with handle " << cast<PlaylistID>(playlist);
    }
}

void AIMPManager36::playlistRemoved(AIMP36SDK::IAIMPPlaylist* playlist)
{
    try {
        BOOST_LOG_SEV(logger(), debug) << "playlistRemoved: id = " << cast<PlaylistID>(playlist);

        const int playlist_id = cast<PlaylistID>(playlist);
        deletePlaylistFromPlaylistDB(playlist_id);
        playlist_helpers_.erase(std::remove_if(playlist_helpers_.begin(), playlist_helpers_.end(),
                                               [playlist](const PlaylistHelper& h) { return h.playlist_.get() == playlist; }
                                               ),
                                playlist_helpers_.end()
                                );
        notifyAllExternalListeners(EVENT_PLAYLISTS_CONTENT_CHANGE);
    } catch (std::exception& e) {
        BOOST_LOG_SEV(logger(), error) << "Error in "__FUNCTION__ << " for playlist with playlist_id " << cast<PlaylistID>(playlist) << ". Reason: " << e.what();
    } catch (...) {
        // we can't propagate exception from here since it is called from AIMP. Just log unknown error.
        BOOST_LOG_SEV(logger(), error) << "Unknown exception in "__FUNCTION__ << " for playlist with playlist_id " << cast<PlaylistID>(playlist);
    }
}

std::string playlist36NotifyFlagsToString(DWORD flags);

void AIMPManager36::playlistChanged(AIMP36SDK::IAIMPPlaylist* playlist, DWORD flags)
{
    try {
        
        BOOST_LOG_SEV(logger(), debug) << "playlistChanged()...: id = " << cast<PlaylistID>(playlist) << ", flags = " << flags << ": " << playlist36NotifyFlagsToString(flags);

        PlaylistID playlist_id = cast<PlaylistID>(playlist);
        bool is_playlist_changed = false;
        if (   (AIMP_PLAYLIST_NOTIFY_NAME       & flags) != 0 
            || (AIMP_PLAYLIST_NOTIFY_FILEINFO   & flags) != 0
            || (AIMP_PLAYLIST_NOTIFY_STATISTICS & flags) != 0 
            )
        {
            BOOST_LOG_SEV(logger(), debug) << "updatePlaylist";
            is_playlist_changed = true;
        }

        if (   (AIMP_PLAYLIST_NOTIFY_FILEINFO & flags) != 0  
            || (AIMP_PLAYLIST_NOTIFY_CONTENT  & flags) != 0 
            )
        {
            BOOST_LOG_SEV(logger(), debug) << "loadEntries";
            loadEntries(playlist); 
            is_playlist_changed = true;
        }

        if (is_playlist_changed) {
            int playlist_index = getPlaylistIndexByHandle(playlist);
            loadPlaylist(playlist, playlist_index);
            updatePlaylistCrcInDB(playlist_id, getPlaylistCRC32(playlist_id));
            notifyAllExternalListeners(EVENT_PLAYLISTS_CONTENT_CHANGE);
        }

        BOOST_LOG_SEV(logger(), debug) << "...playlistChanged()";
    } catch (std::exception& e) {
        BOOST_LOG_SEV(logger(), error) << "Error in "__FUNCTION__ << " for playlist with id " << cast<PlaylistID>(playlist) << ". Reason: " << e.what();
    } catch (...) {
        // we can't propagate exception from here since it is called from AIMP. Just log unknown error.
        BOOST_LOG_SEV(logger(), error) << "Unknown exception in "__FUNCTION__ << " for playlist with id " << cast<PlaylistID>(playlist);
    }
}

class AIMPManager36::AIMPPlaylistListener : public AIMP36SDK::IUnknownInterfaceImpl<AIMP36SDK::IAIMPPlaylistListener>
{
public:
    explicit AIMPPlaylistListener(boost::intrusive_ptr<IAIMPPlaylist> playlist, AIMPManager36* aimp36_manager)
        : 
        aimp36_manager_(aimp36_manager),
        playlist_(playlist)
    {}

    virtual void WINAPI Activated() {}
    virtual void WINAPI Changed(DWORD flags)
        { aimp36_manager_->playlistChanged(playlist_.get(), flags); }
    virtual void WINAPI Removed() {} // use playlistRemoved instead.

    virtual HRESULT WINAPI QueryInterface(REFIID riid, LPVOID* ppvObj) {
        if (!ppvObj) {
            return E_POINTER;
        }

        if (IID_IUnknown == riid) {
            *ppvObj = this;
            AddRef();
            return S_OK;
        } else if (AIMP36SDK::IID_IAIMPPlaylistListener == riid) {
            *ppvObj = this;
            AddRef();
            return S_OK;                
        }

        return E_NOINTERFACE;
    }

private:

    boost::intrusive_ptr<IAIMPPlaylist> playlist_;
    AIMPManager36* aimp36_manager_;
};

namespace {
// On error it prints error reason to log only.
void executeQuery(const std::string& query, sqlite3* db, const char* log_tag)
{
    char* errmsg = nullptr;
    const int rc = sqlite3_exec(db,
                                query.c_str(),
                                nullptr, /* Callback function */
                                nullptr, /* 1st argument to callback */
                                &errmsg
                                );
    if (SQLITE_OK != rc) {
        BOOST_LOG_SEV(logger(), error) << log_tag << " failed. Reason: sqlite3_exec() error "
                                       << rc << ": " << errmsg 
                                       << ". Query: " << query;
        sqlite3_free(errmsg);
    }
}
} // namespace

void AIMPManager36::deletePlaylistFromPlaylistDB(PlaylistID playlist_id)
{
    deletePlaylistEntriesFromPlaylistDB(playlist_id);

    const std::string query = MakeString() << "DELETE FROM Playlists WHERE id=" << playlist_id;

    executeQuery(query, playlists_db_, __FUNCTION__);
}

void AIMPManager36::deletePlaylistEntriesFromPlaylistDB(PlaylistID playlist_id)
{
    const std::string query = MakeString() << "DELETE FROM PlaylistsEntries WHERE playlist_id=" << playlist_id;

    executeQuery(query, playlists_db_, __FUNCTION__);
}

void releasePlaylistItems(sqlite3* playlists_db, const std::string& query)
{
    sqlite3_stmt* stmt = createStmt( playlists_db, query.c_str() );
    ON_BLOCK_EXIT(&sqlite3_finalize, stmt);

    for(;;) {
		int rc_db = sqlite3_step(stmt);
        if (SQLITE_ROW == rc_db) {
            const int entry_id = sqlite3_column_int(stmt, 0);
            if (IAIMPPlaylistItem* item = castToPlaylistItem(entry_id)) {
                item->Release();
            }
        } else if (SQLITE_DONE == rc_db) {
            break;
        } else {
            const std::string msg = MakeString() << "sqlite3_step() error "
                                                 << rc_db << ": " << sqlite3_errmsg(playlists_db)
                                                 << ". Query: " << query;
            throw std::runtime_error(msg);
		}
    }
}

AIMPManager36::PlaylistHelper::PlaylistHelper(IAIMPPlaylist_ptr playlist, AIMPManager36* aimp36_manager)
    : playlist_(playlist),
      crc32_(cast<PlaylistID>(playlist.get()), aimp36_manager->playlists_db()),
      listener_(new AIMPPlaylistListener(playlist.get(), aimp36_manager))
{
    playlist_->ListenerAdd(listener_.get());
}

AIMPManager36::PlaylistHelper::~PlaylistHelper()
{
    playlist_->ListenerRemove(listener_.get());
}

void AIMPManager36::loadPlaylist(IAIMPPlaylist* playlist, int playlist_index)
{
    const PlaylistID playlist_id = cast<PlaylistID>(playlist);

    getPlaylistCRC32Object(playlist_id).reset_properties();

    const char * const error_prefix = "Error occured while extracting playlist data: ";

    IAIMPPropertyList* playlist_propertylist_tmp;
    HRESULT r = playlist->QueryInterface(IID_IAIMPPropertyList,
                                         reinterpret_cast<void**>(&playlist_propertylist_tmp));
    if (S_OK != r) {
        throw std::runtime_error(MakeString() << error_prefix << "playlist->QueryInterface(IID_IAIMPPropertyList) failed. Result " << r);
    }
    boost::intrusive_ptr<IAIMPPropertyList> playlist_propertylist(playlist_propertylist_tmp, false);
    playlist_propertylist_tmp = nullptr;

    INT64 size = 0;
    r = playlist_propertylist->GetValueAsInt64(AIMP_PLAYLIST_PROPID_SIZE, &size);
    if (S_OK != r) {
        throw std::runtime_error(MakeString() << error_prefix << "IAIMPPropertyList::GetValueAsInt64(AIMP_PLAYLIST_PROPID_SIZE) failed. Result " << r);
    }

    double duration_tmp;
    r = playlist_propertylist->GetValueAsFloat(AIMP_PLAYLIST_PROPID_DURATION, &duration_tmp);
    if (S_OK != r) {
        throw std::runtime_error(MakeString() << error_prefix << "IAIMPPropertyList::GetValueAsFloat(AIMP_PLAYLIST_PROPID_DURATION) failed. Result " << r);
    }
    const INT64 duration = static_cast<INT64>(duration_tmp);

    IAIMPString* name_tmp;
    r = playlist_propertylist->GetValueAsObject(AIMP_PLAYLIST_PROPID_NAME, IID_IAIMPString, reinterpret_cast<void**>(&name_tmp));
    if (S_OK != r) {
        throw std::runtime_error(MakeString() << error_prefix << "IAIMPAddonsPlaylistManager::StoragePropertyGetValue(AIMP_PLAYLIST_STORAGE_PROPERTY_NAME) failed. Result " << r);
    }
    boost::intrusive_ptr<IAIMPString> nameStr(name_tmp, false);
    const WCHAR* name = nameStr->GetData();

    const int entries_count = playlist->GetItemCount();

    { // db code
    sqlite3_stmt* stmt = createStmt(playlists_db_,
                                    "REPLACE INTO Playlists VALUES (?,?,?,?,?,?,?)"
                                    );
    ON_BLOCK_EXIT(&sqlite3_finalize, stmt);

#define bind(type, field_index, value)  rc_db = sqlite3_bind_##type(stmt, field_index, value); \
                                        if (SQLITE_OK != rc_db) { \
                                            const std::string msg = MakeString() << "Error sqlite3_bind_"#type << " " << rc_db; \
                                            throw std::runtime_error(msg); \
                                        }
#define bindText(field_index, text, textLength) rc_db = sqlite3_bind_text16(stmt, field_index, text, textLength * sizeof(WCHAR), SQLITE_STATIC); \
                                                if (SQLITE_OK != rc_db) { \
                                                    const std::string msg = MakeString() << "sqlite3_bind_text16" << " " << rc_db; \
                                                    throw std::runtime_error(msg); \
                                                }

    int rc_db;
    bind(int,   1, playlist_id);
    bind(int,   2, playlist_index);
    bindText(   3, name, nameStr->GetLength());
    bind(int,   4, entries_count);
    bind(int64, 5, duration);
    bind(int64, 6, size);
    bind(int64, 7, kCRC32_UNINITIALIZED);
#undef bind
#undef bindText
    rc_db = sqlite3_step(stmt);
    if (SQLITE_DONE != rc_db) {
        const std::string msg = MakeString() << "sqlite3_step() error "
                                             << rc_db << ": " << sqlite3_errmsg(playlists_db_);
        throw std::runtime_error(msg);
    }
    }
}

namespace Support {

HRESULT getString(IAIMPPropertyList* property_list, const int property_id, AIMPString_ptr* value)
{
    assert(property_list);
    assert(value);

    IAIMPString* value_tmp;
    HRESULT r = property_list->GetValueAsObject(property_id, IID_IAIMPString, reinterpret_cast<void**>(&value_tmp));
    if (S_OK == r) {
        AIMPString_ptr(value_tmp, false).swap(*value);

#ifndef NDEBUG
        BOOST_LOG_SEV(logger(), debug) << "getString(property_id = " << property_id << ") value: " << StringEncoding::utf16_to_utf8(value_tmp->GetData(), value_tmp->GetData() + value_tmp->GetLength());
#endif
    }
    return r;
}

AIMPString_ptr getString(IAIMPPropertyList* property_list, const int property_id, const char* error_prefix)
{
    assert(error_prefix);

    AIMPString_ptr value;
    HRESULT r = getString(property_list, property_id, &value);
    if (S_OK != r) {
        throw std::runtime_error(MakeString() << error_prefix << ": getString(): property_list->GetValueAsObject(property id = " << property_id << ") failed. Result " << r);
    }
    return value;
}

HRESULT getString(IAIMPFileInfo* file_info, const int property_id, AIMPString_ptr* value)
{
    assert(file_info);
    assert(value);

    IAIMPString* value_tmp;
    HRESULT r = file_info->GetValueAsObject(property_id, IID_IAIMPString, reinterpret_cast<void**>(&value_tmp));
    if (S_OK == r) {
        AIMPString_ptr(value_tmp, false).swap(*value);

#ifndef NDEBUG
        BOOST_LOG_SEV(logger(), debug) << "getString(property_id = " << AIMP36SDK::Support::FileinfoPropIdToString(property_id) << ") value: " << StringEncoding::utf16_to_utf8(value_tmp->GetData(), value_tmp->GetData() + value_tmp->GetLength());
#endif
    }
    return r;
}

AIMPString_ptr getString(IAIMPFileInfo* file_info, const int property_id, const char* error_prefix)
{
    assert(error_prefix);

    AIMPString_ptr value;
    HRESULT r = getString(file_info, property_id, &value);
    if (S_OK != r) {
        throw std::runtime_error(MakeString() << error_prefix << ": getString(): file_info->GetValueAsObject(property id = " << AIMP36SDK::Support::FileinfoPropIdToString(property_id) << ") failed. Result " << r);
    }
    return value;
}

HRESULT getString(IAIMPPlaylistItem* playlist_item, const int property_id, AIMPString_ptr* value)
{
    assert(playlist_item);
    assert(value);

    IAIMPString* value_tmp;
    HRESULT r = playlist_item->GetValueAsObject(property_id, IID_IAIMPString, reinterpret_cast<void**>(&value_tmp));
    if (S_OK == r) {
        AIMPString_ptr(value_tmp, false).swap(*value);

#ifndef NDEBUG
        BOOST_LOG_SEV(logger(), debug) << "getString(property_id = " << AIMP36SDK::Support::PlaylistItemToString(property_id) << ") value: " << StringEncoding::utf16_to_utf8(value_tmp->GetData(), value_tmp->GetData() + value_tmp->GetLength());
#endif
    }
    return r;
}

AIMPString_ptr getString(IAIMPPlaylistItem* playlist_item, const int property_id, const char* error_prefix)
{
    assert(error_prefix);

    AIMPString_ptr value;
    HRESULT r = getString(playlist_item, property_id, &value);
    if (S_OK != r) {
        throw std::runtime_error(MakeString() << error_prefix << ": getString(): playlist_item->GetValueAsObject(property id = " << AIMP36SDK::Support::PlaylistItemToString(property_id) << ") failed. Result " << r);
    }
    return value;
}

HRESULT getInt(IAIMPFileInfo* file_info, const int property_id, int* value)
{
    assert(file_info);
    assert(value);

    int value_tmp;
    HRESULT r = file_info->GetValueAsInt32(property_id, &value_tmp);
    if (S_OK == r) {
        *value = value_tmp;

#ifndef NDEBUG
        BOOST_LOG_SEV(logger(), debug) << "getInt(property_id = " << AIMP36SDK::Support::FileinfoPropIdToString(property_id) << ") value: " << *value;
#endif
    }
    return r;
}

int getInt(IAIMPFileInfo* file_info, const int property_id, const char* error_prefix)
{
    assert(error_prefix);

    int value;
    HRESULT r = getInt(file_info, property_id, &value);
    if (S_OK != r) {
        throw std::runtime_error(MakeString() << error_prefix << ": getInt(): file_info->GetValueAsInt32(property id = " << AIMP36SDK::Support::FileinfoPropIdToString(property_id) << ") failed. Result " << r);
    }
    return value;
}

HRESULT getInt64(IAIMPFileInfo* file_info, const int property_id, INT64* value)
{
    assert(file_info);
    assert(value);

    INT64 value_tmp;
    HRESULT r = file_info->GetValueAsInt64(property_id, &value_tmp);
    if (S_OK == r) {
        *value = value_tmp;

#ifndef NDEBUG
        BOOST_LOG_SEV(logger(), debug) << "getInt64(property_id = " << AIMP36SDK::Support::FileinfoPropIdToString(property_id) << ") value: " << *value;
#endif
    }
    return r;
}

INT64 getInt64(IAIMPFileInfo* file_info, const int property_id, const char* error_prefix)
{
    assert(error_prefix);

    INT64 value;
    HRESULT r = getInt64(file_info, property_id, &value);
    if (S_OK != r) {
        throw std::runtime_error(MakeString() << error_prefix << ": getInt64(): file_info->GetValueAsInt64(property id = " << AIMP36SDK::Support::FileinfoPropIdToString(property_id) << ") failed. Result " << r);
    }
    return value;
}

HRESULT getDouble(IAIMPFileInfo* file_info, const int property_id, double* value)
{
    assert(file_info);
    assert(value);

    double value_tmp;
    HRESULT r = file_info->GetValueAsFloat(property_id, &value_tmp);
    if (S_OK == r) {
        *value = value_tmp;

#ifndef NDEBUG
        BOOST_LOG_SEV(logger(), debug) << "getDouble(property_id = " << AIMP36SDK::Support::FileinfoPropIdToString(property_id) << ") value: " << *value;
#endif
    }
    return r;
}

double getDouble(IAIMPFileInfo* file_info, const int property_id, const char* error_prefix)
{
    assert(error_prefix);

    double value;
    HRESULT r = getDouble(file_info, property_id, &value);
    if (S_OK != r) {
        throw std::runtime_error(MakeString() << error_prefix << ": getDouble(): file_info->GetValueAsFloat(property id = " << AIMP36SDK::Support::FileinfoPropIdToString(property_id) << ") failed. Result " << r);
    }
    return value;
}

} // namespace Support

void AIMPManager36::loadEntries(IAIMPPlaylist* playlist)
{
    using namespace Support;
    PROFILE_EXECUTION_TIME(__FUNCTION__);

    PlaylistID playlist_id = cast<PlaylistID>(playlist);

    { // handle crc32.
        try {
            getPlaylistCRC32Object(playlist_id).reset_entries();
        } catch (std::exception& e) {
            throw std::runtime_error(MakeString() << "expected crc32 struct for playlist " << playlist_id << " not found in "__FUNCTION__". Reason: " << e.what());
        }
    }

    const int entries_count = playlist->GetItemCount();

    deletePlaylistEntriesFromPlaylistDB(playlist_id); // remove old entries before adding new ones.
    PlaylistItems& entry_ids = getPlaylistHelper(playlist).entry_ids_;
    entry_ids.clear();
    entry_ids.reserve(entries_count);

    sqlite3_stmt* stmt = createStmt(playlists_db_, "INSERT INTO PlaylistsEntries VALUES (?,?,?,?,?,?,"
                                                                                        "?,?,?,?,?,"
                                                                                        "?,?,?,?,?)"
                                    );
    ON_BLOCK_EXIT(&sqlite3_finalize, stmt);

    //BOOST_LOG_SEV(logger(), debug) << "The statement has "
    //                               << sqlite3_bind_parameter_count(stmt)
    //                               << " wildcards";
    
    const char * const error_prefix = "Error occured while extracting playlist item data: ";
    
    for (int item_index = 0; item_index < entries_count; ++item_index) {
        IAIMPPlaylistItem* item_tmp;
        HRESULT r = playlist->GetItem(item_index,
                                      IID_IAIMPPlaylistItem,
                                      reinterpret_cast<void**>(&item_tmp)
                                      );
        if (S_OK != r) {
            throw std::runtime_error(MakeString() << error_prefix << "playlist->GetItem(IID_IAIMPPlaylistItem) failed. Result " << r);
        }
        boost::intrusive_ptr<IAIMPPlaylistItem> item(item_tmp, false); 
        item_tmp = nullptr;

        AIMPString_ptr album,
                       artist,
                       date,
                       fileName,
                       genre,
                       title;

        int bitrate = 0,
            channels = 0,
            duration = 0,
            rating = 0,
            samplerate = 0;
        int64_t filesize = 0;

        IAIMPFileInfo* file_info_tmp;
        r = item->GetValueAsObject(AIMP_PLAYLISTITEM_PROPID_FILEINFO, IID_IAIMPFileInfo,
                                   reinterpret_cast<void**>(&file_info_tmp)
                                   );
        if (S_OK == r) {
            boost::intrusive_ptr<IAIMPFileInfo> file_info(file_info_tmp, false);
            file_info_tmp = nullptr;

            album    = getString(file_info.get(), AIMP_FILEINFO_PROPID_ALBUM,    error_prefix);
            artist   = getString(file_info.get(), AIMP_FILEINFO_PROPID_ARTIST,   error_prefix);
            date     = getString(file_info.get(), AIMP_FILEINFO_PROPID_DATE,     error_prefix);
            fileName = getString(file_info.get(), AIMP_FILEINFO_PROPID_FILENAME, error_prefix);
            genre    = getString(file_info.get(), AIMP_FILEINFO_PROPID_GENRE,    error_prefix);
            title    = getString(file_info.get(), AIMP_FILEINFO_PROPID_TITLE,    error_prefix);
            if (!title || title->GetLength() == 0) {
                title = getString(item.get(), AIMP_PLAYLISTITEM_PROPID_DISPLAYTEXT, error_prefix); // title should not be empty.
            }

            bitrate    = getInt(file_info.get(), AIMP_FILEINFO_PROPID_BITRATE,    error_prefix);
            channels   = getInt(file_info.get(), AIMP_FILEINFO_PROPID_CHANNELS,   error_prefix);
            samplerate = getInt(file_info.get(), AIMP_FILEINFO_PROPID_SAMPLERATE, error_prefix);

            duration = static_cast<int>(getDouble(file_info.get(), AIMP_FILEINFO_PROPID_DURATION, error_prefix));
            rating   = static_cast<int>(getDouble(file_info.get(), AIMP_FILEINFO_PROPID_MARK,     error_prefix));

            filesize = getInt64(file_info.get(), AIMP_FILEINFO_PROPID_FILESIZE, error_prefix);
        } else {
            // This item is not of IAIMPFileInfo type.
            IAIMPVirtualFile* virtual_file_tmp;
            r = item->QueryInterface(IID_IAIMPVirtualFile,
                                     reinterpret_cast<void**>(&virtual_file_tmp)
                                     );
            if (S_OK == r) {
                boost::intrusive_ptr<IAIMPVirtualFile> virtual_file(virtual_file_tmp, false);
                virtual_file_tmp = nullptr;
            } else {
                // This item is not of IAIMPFileInfo/IAIMPVirtualFile types.
                // Use PLAYLISTITEM fields
                            /*
                    const int AIMP_PLAYLISTITEM_PROPID_DISPLAYTEXT    = 1; <- used when AIMP_FILEINFO_PROPID_TITLE is empty or does not exits.
                    const int AIMP_PLAYLISTITEM_PROPID_FILEINFO       = 2;
                    const int AIMP_PLAYLISTITEM_PROPID_FILENAME       = 3;
                    const int AIMP_PLAYLISTITEM_PROPID_GROUP          = 4;
                    const int AIMP_PLAYLISTITEM_PROPID_INDEX          = 5;
                    const int AIMP_PLAYLISTITEM_PROPID_MARK           = 6;
                    const int AIMP_PLAYLISTITEM_PROPID_PLAYINGSWITCH  = 7;
                    const int AIMP_PLAYLISTITEM_PROPID_PLAYLIST       = 8;
                    const int AIMP_PLAYLISTITEM_PROPID_SELECTED       = 9;
                    const int AIMP_PLAYLISTITEM_PROPID_PLAYBACKQUEUEINDEX = 10;
                            */

            }
        }

        const int entry_id = castToPlaylistEntryID(item.get());

#ifndef NDEBUG
        BOOST_LOG_SEV(logger(), debug) << "index: " << item_index << ", entry_id: " << entry_id;
#endif



        { // special db code
            // bind all values
        
#define bind(type, field_index, value)  rc_db = sqlite3_bind_##type(stmt, field_index, value); \
                                        if (SQLITE_OK != rc_db) { \
                                            const std::string msg = MakeString() << "Error sqlite3_bind_"#type << " " << rc_db; \
                                            throw std::runtime_error(msg); \
                                        }

#define bindText(field_index, aimp_string)  rc_db = sqlite3_bind_text16(stmt, field_index, aimp_string->GetData(), aimp_string->GetLength() * sizeof(WCHAR), SQLITE_STATIC); \
                                            if (SQLITE_OK != rc_db) { \
                                                const std::string msg = MakeString() << "sqlite3_bind_text16 rc_db: " << rc_db; \
                                                throw std::runtime_error(msg); \
                                            }
            int rc_db;
            bind(int,    1, playlist_id);
            bind(int,    2, entry_id);
            bind(int,    3, item_index);

            if (album)    { bindText(4, album); }
            if (artist)   { bindText(5, artist); }
            if (date)     { bindText(6, date); }
            if (fileName) { bindText(7, fileName); }
            if (genre)    { bindText(8, genre); }
            if (title)    { bindText(9, title); }

#undef bindText
            bind(int,   10, bitrate);
            bind(int,   11, channels);
            bind(int,   12, duration);
            bind(int64, 13, filesize);
            bind(int,   14, rating);
            bind(int,   15, samplerate);
            bind(int64, 16, crc32(info));
#undef bind

            rc_db = sqlite3_step(stmt);
            if (SQLITE_DONE != rc_db) {
                const std::string msg = MakeString() << "sqlite3_step() error "
                                                     << rc_db << ": " << sqlite3_errmsg(playlists_db_);
                throw std::runtime_error(msg);
            } else {
                entry_ids.push_back(item.get()); // notice that we taking ownership to access to items later.
            }
            sqlite3_reset(stmt);
        }
    }

    // sort entry ids to use binary search later
    std::sort(entry_ids.begin(), entry_ids.end());
}

int AIMPManager36::getPlaylistIndexByHandle(IAIMPPlaylist* playlist)
{
    for (int i = 0, count = aimp_service_playlist_manager_->GetLoadedPlaylistCount(); i != count; ++i) {
        IAIMPPlaylist* current_playlist;
        HRESULT r = aimp_service_playlist_manager_->GetLoadedPlaylist(i, &current_playlist);
        if (S_OK != r) {
            throw std::runtime_error(MakeString() << "GetLoadedPlaylist failure: " << r);
        }
        current_playlist->Release();
        
        if (playlist == current_playlist) {
            return i;
        }
    }

    return -1;
}

AIMPManager36::PlaylistHelper& AIMPManager36::getPlaylistHelper(IAIMPPlaylist* playlist)
{
    assert(playlist);
    for (auto& helper : playlist_helpers_) {
        if (helper.playlist_.get() == playlist) {
            return helper;
        }
    }

    throw std::runtime_error(MakeString() << __FUNCTION__": playlist with id " << cast<PlaylistID>(playlist) << "is not found");
}

IAIMPPlaylistItem_ptr AIMPManager36::getPlaylistItem(PlaylistEntryID id) const
{
    IAIMPPlaylistItem* to_search = castToPlaylistItem(id);
    for (auto& helper : playlist_helpers_) {
        const PlaylistItems& entry_ids = helper.entry_ids_;
        const auto& begin_it = entry_ids.begin();
        const auto& end_it = entry_ids.end();
        PlaylistItems::const_iterator it = std::lower_bound(begin_it, end_it,
                                                            to_search,
                                                            [](const IAIMPPlaylistItem_ptr& element, IAIMPPlaylistItem* to_search) { return element.get() < to_search; }
                                                            );
        if (it != end_it && !(to_search < (*begin_it).get())) {
            return *it;
        }
    }

    return IAIMPPlaylistItem_ptr();
}

AIMP36SDK::IAIMPPlaylistItem_ptr AIMPManager36::getPlaylistItem(PlaylistEntryID id)
{
    return (const_cast<const AIMPManager36*>(this)->getPlaylistItem(id));
}

void AIMPManager36::notifyAllExternalListeners(AIMPManager::EVENTS event) const
{
    for (const auto& listener_pair : external_listeners_) {
        const EventsListener& listener = listener_pair.second;
        listener(event);
    }
}

AIMPManager::EventsListenerID AIMPManager36::registerListener(AIMPManager::EventsListener listener)
{
    external_listeners_[next_listener_id_] = listener;
    assert(next_listener_id_ != UINT32_MAX);
    return ++next_listener_id_; // get next unique listener ID using simple increment.
}

void AIMPManager36::unRegisterListener(AIMPManager::EventsListenerID listener_id)
{
    external_listeners_.erase(listener_id);
}

void AIMPManager36::startPlayback()
{
    switch (getPlaybackState()) {
    case PAUSED: {
        HRESULT r = aimp_service_player_->Resume();
        if (S_OK != r) {
            throw std::runtime_error( MakeString() << __FUNCTION__": aimp_service_player_->Resume() failed. Result: " << r);
        }
        break;
    }
    case PLAYING:
        // do nothing
        break;
    case STOPPED:
        startPlayback(TrackDescription(-1, -1));
        break;
    }
}

void AIMPManager36::startPlayback(TrackDescription track_desc)
{
    TrackDescription absolute_track_desc(getAbsoluteTrackDesc(track_desc));
    if (IAIMPPlaylistItem_ptr item = getPlaylistItem(absolute_track_desc.track_id)) {
        HRESULT r = aimp_service_player_->Play2(item.get());
        if (S_OK != r) {
            throw std::runtime_error( MakeString() << __FUNCTION__": aimp_service_player_->Play2 failed for track " << track_desc << ". Result: " << r);
        }
    } else {
        throw std::runtime_error( MakeString() << __FUNCTION__": invalid track" << track_desc);
    }
}

void AIMPManager36::stopPlayback()
{
    HRESULT r = aimp_service_player_->Stop();
    if (S_OK != r) {
        throw std::runtime_error( MakeString() << __FUNCTION__": aimp_service_player_->Stop() failed. Result: " << r);
    }
}

std::string AIMPManager36::getAIMPVersion() const
{
    IAIMPServiceVersionInfo* version_info_tmp;
    HRESULT r = aimp36_core_->QueryInterface(IID_IAIMPServiceVersionInfo, reinterpret_cast<void**>(&version_info_tmp));
    if (S_OK != r) {
        BOOST_LOG_SEV(logger(), error) << "aimp36_core_->QueryInterface(IID_IAIMPServiceVersionInfo) falied. Result: " << r;
        return "";
    }
    boost::intrusive_ptr<IAIMPServiceVersionInfo> version_info(version_info_tmp, false);

    std::ostringstream os;

    IAIMPString* info_tmp;
    r = version_info->FormatInfo(&info_tmp);
    if (S_OK == r) {
        AIMPString_ptr info(info_tmp, false);
        os << ' ' << StringEncoding::utf16_to_system_ansi_encoding_safe(info->GetData());
    } else {
        using namespace std;
        const int version = version_info->GetVersionID();
        os << version / 1000 << '.' << setfill('0') << setw(2) << (version % 1000) / 10 << '.' << version % 10
           << " Build " << version_info->GetBuildNumber();   
    }
    
    std::string result(os.str());
    boost::algorithm::trim(result);
    return result;
}

void AIMPManager36::pausePlayback()
{
    HRESULT r = aimp_service_player_->Pause();
    if (S_OK != r) {
        throw std::runtime_error( MakeString() << __FUNCTION__": aimp_service_player_->Pause() failed. Result: " << r);
    }
}

void AIMPManager36::playNextTrack()
{
    HRESULT r = aimp_service_player_->GoToNext();
    if (S_OK != r) {
        throw std::runtime_error( MakeString() << __FUNCTION__": aimp_service_player_->GoToNext() failed. Result: " << r);
    }
}

void AIMPManager36::playPreviousTrack()
{
    HRESULT r = aimp_service_player_->GoToPrev();
    if (S_OK != r) {
        throw std::runtime_error( MakeString() << __FUNCTION__": aimp_service_player_->GoToPrev() failed. Result: " << r);
    }
}

void AIMPManager36::onAimpCoreMessage(DWORD AMessage, int AParam1, void* /*AParam2*/, HRESULT* AResult)
{
    assert(AResult);
    /*
        ///!!! do not know when to notify about these events.
        EVENT_INFO_UPDATE,
        EVENT_EFFECT_CHANGED,
    */
    switch (AMessage) {
    case AIMP_MSG_EVENT_PLAYER_STATE:
        notifyAllExternalListeners(EVENT_PLAYER_STATE);
        break;
    case AIMP_MSG_EVENT_PLAYER_UPDATE_POSITION:
        //notifyAllExternalListeners(EVENT_PLAY_FILE);
        break;
    case AIMP_MSG_EVENT_STREAM_START:
        notifyAllExternalListeners(EVENT_TRACK_PROGRESS_CHANGED_DIRECTLY);
        break;
    case AIMP_MSG_CMD_QUIT:
        notifyAllExternalListeners(EVENT_AIMP_QUIT);
        break;
    case AIMP_MSG_EVENT_PROPERTY_VALUE: {
        const int property_id = AParam1;
        switch (property_id) {
        case AIMP_MSG_PROPERTY_VOLUME:
            notifyAllExternalListeners(EVENT_VOLUME);
            break;
        case AIMP_MSG_PROPERTY_MUTE:
            notifyAllExternalListeners(EVENT_MUTE);
            break;
        case AIMP_MSG_PROPERTY_SHUFFLE:
            notifyAllExternalListeners(EVENT_SHUFFLE);
            break;
        case AIMP_MSG_PROPERTY_REPEAT:
            notifyAllExternalListeners(EVENT_REPEAT);
            break;
        case AIMP_MSG_PROPERTY_EQUALIZER:
        case AIMP_MSG_PROPERTY_EQUALIZER_BAND:
            notifyAllExternalListeners(EVENT_EQ_CHANGED);
            break;
        case AIMP_MSG_PROPERTY_PLAYER_POSITION:
            notifyAllExternalListeners(EVENT_TRACK_POS_CHANGED);
            break;
        case AIMP_MSG_PROPERTY_RADIOCAP:
            notifyAllExternalListeners(EVENT_RADIO_CAPTURE);
            break;
        default:
            notifyAllExternalListeners(EVENT_STATUS_CHANGE);
            break;
        }
        break;
    }
    default:
        AMessage = AMessage;
        break;
    }
    *AResult = E_NOTIMPL;
}

namespace
{

///!!! TODO check it? For some reason IAIMPCoreUnit::MessageSend() returns -1 instead 1 for BOOL.
AIMPManager::StatusValue patchBool(BOOL value)
{
    return value ? 1 : 0;
}

} // namespace

AIMPManager::StatusValue AIMPManager36::getStatus(AIMPManager::STATUS status) const
{
    DWORD msg = 0;
    const int param1 = AIMP_MSG_PROPVALUE_GET;
    HRESULT r = S_OK;
    switch (status) {
    case STATUS_VOLUME: {
        msg = AIMP_MSG_PROPERTY_VOLUME;
        float value;
        r = aimp_service_message_dispatcher_->Send(msg, param1, &value);
        if (S_OK == r) {
            return static_cast<StatusValue>(value * 100.f);
        }
        break;
    }
    case STATUS_BALANCE: {
        msg = AIMP_MSG_PROPERTY_BALANCE;
        float value;
        r = aimp_service_message_dispatcher_->Send(msg, param1, &value);
        if (S_OK == r) {
            return static_cast<StatusValue>((value + 1.f)*50.f);
        }
        break;
    }
    case STATUS_SPEED: {
        msg = AIMP_MSG_PROPERTY_SPEED;
        float value;
        r = aimp_service_message_dispatcher_->Send(msg, param1, &value);
        if (S_OK == r) {
            return static_cast<StatusValue>((value - 0.5f) * 100.f);
        }
        break;
    }
    case STATUS_Player: {
        msg = AIMP_MSG_PROPERTY_PLAYER_STATE;
        int value;
        r = aimp_service_message_dispatcher_->Send(msg, param1, &value);
        if (S_OK == r) {
            switch (value) {
            case 0: return STOPPED;
            case 1: return PAUSED;
            case 2: return PLAYING;
            }
        }
        break;
    }
    case STATUS_MUTE: {
        msg = AIMP_MSG_PROPERTY_MUTE;
        BOOL value;
        r = aimp_service_message_dispatcher_->Send(msg, param1, &value);
        if (S_OK == r) {
            return patchBool(value);
        }
        break;
    }
    case STATUS_REVERB: {
        msg = AIMP_MSG_PROPERTY_REVERB;
        float value;
        r = aimp_service_message_dispatcher_->Send(msg, param1, &value);
        if (S_OK == r) {
            return static_cast<StatusValue>(value * 100.f);
        }
        break;
    }
    case STATUS_ECHO: {
        msg = AIMP_MSG_PROPERTY_ECHO;
        float value;
        r = aimp_service_message_dispatcher_->Send(msg, param1, &value);
        if (S_OK == r) {
            return static_cast<StatusValue>(value * 100.f);
        }
        break;
    }
    case STATUS_CHORUS: {
        msg = AIMP_MSG_PROPERTY_CHORUS;
        float value;
        r = aimp_service_message_dispatcher_->Send(msg, param1, &value);
        if (S_OK == r) {
            return static_cast<StatusValue>(value * 100.f);
        }
        break;
    }
    case STATUS_Flanger: {
        msg = AIMP_MSG_PROPERTY_FLANGER;
        float value;
        r = aimp_service_message_dispatcher_->Send(msg, param1, &value);
        if (S_OK == r) {
            return static_cast<StatusValue>(value * 100.f);
        }
        break;
    }
    case STATUS_EQ_STS: {
        msg = AIMP_MSG_PROPERTY_EQUALIZER;
        BOOL value;
        r = aimp_service_message_dispatcher_->Send(msg, param1, &value);
        if (S_OK == r) {
            return patchBool(value);
        }
        break;
    }
    case STATUS_EQ_SLDR01:
    case STATUS_EQ_SLDR02:
    case STATUS_EQ_SLDR03:
    case STATUS_EQ_SLDR04:
    case STATUS_EQ_SLDR05:
    case STATUS_EQ_SLDR06:
    case STATUS_EQ_SLDR07:
    case STATUS_EQ_SLDR08:
    case STATUS_EQ_SLDR09:
    case STATUS_EQ_SLDR10:
    case STATUS_EQ_SLDR11:
    case STATUS_EQ_SLDR12:
    case STATUS_EQ_SLDR13:
    case STATUS_EQ_SLDR14:
    case STATUS_EQ_SLDR15:
    case STATUS_EQ_SLDR16:
    case STATUS_EQ_SLDR17:
    case STATUS_EQ_SLDR18: {
        msg = AIMP_MSG_PROPERTY_EQUALIZER_BAND;
        const int param1 = MAKELONG(AIMP_MSG_PROPVALUE_GET, status - STATUS_EQ_SLDR01);
        float value;
        r = aimp_service_message_dispatcher_->Send(msg, param1, &value);
        if (S_OK == r) {
            return static_cast<StatusValue>((value + 15.f) / 30.f * 100.f);
        }
        break;
    }
    case STATUS_REPEAT: {
        msg = AIMP_MSG_PROPERTY_REPEAT;
        BOOL value;
        r = aimp_service_message_dispatcher_->Send(msg, param1, &value);
        if (S_OK == r) {
            return patchBool(value);
        }
        break;
    }
    case STATUS_STAY_ON_TOP: {
        msg = AIMP_MSG_PROPERTY_STAYONTOP;
        BOOL value;
        r = aimp_service_message_dispatcher_->Send(msg, param1, &value);
        if (S_OK == r) {
            return patchBool(value);
        }
        break;                         
    }
    case STATUS_POS: {
        msg = AIMP_MSG_PROPERTY_PLAYER_POSITION;
        float value;
        r = aimp_service_message_dispatcher_->Send(msg, param1, &value);
        if (S_OK == r) {
            return static_cast<StatusValue>(value);
        }
        break;
    }
    case STATUS_LENGTH: {
        msg = AIMP_MSG_PROPERTY_PLAYER_DURATION;
        float value;
        r = aimp_service_message_dispatcher_->Send(msg, param1, &value);
        if (S_OK == r) {
            return static_cast<StatusValue>(value);
        }
        break;
    }
    case STATUS_ACTION_ON_END_OF_PLAYLIST: {
        msg = AIMP_MSG_PROPERTY_ACTION_ON_END_OF_PLAYLIST;
        int value;
        r = aimp_service_message_dispatcher_->Send(msg, param1, &value);
        if (S_OK == r) {
            enum { JUMP_TO_THE_NEXT_PLAYLIST,
                   REPEAT_PLAYLIST,
                   DO_NOTHING };
            
            return value;
        }
        break;
    }
    case STATUS_REPEAT_SINGLE_FILE_PLAYLISTS: {
        msg = AIMP_MSG_PROPERTY_REPEAT_SINGLE_FILE_PLAYLISTS;
        BOOL value;
        r = aimp_service_message_dispatcher_->Send(msg, param1, &value);
        if (S_OK == r) {
            return patchBool(value);
        }
        break;
    }
    //STATUS_MODE,
    case STATUS_KBPS:
    case STATUS_KHZ: {
        const char* field = status == STATUS_KBPS ? "bitrate" : "samplerate";
        return getEntryField<DWORD>(playlists_db_, field, getPlayingEntry());
        }
        break;
    //STATUS_STREAM_TYPE,
    case STATUS_REVERSETIME:
    case STATUS_SHUFFLE:
    case STATUS_RADIO_CAPTURE: {
        switch (status) {
        case STATUS_REVERSETIME:   msg = AIMP_MSG_PROPERTY_REVERSETIME; break;
        case STATUS_SHUFFLE:       msg = AIMP_MSG_PROPERTY_SHUFFLE;     break;
        case STATUS_RADIO_CAPTURE: msg = AIMP_MSG_PROPERTY_RADIOCAP;    break;
        default:
            assert(!"unknown status");
            break;
        }
        BOOL value;
        r = aimp_service_message_dispatcher_->Send(msg, param1, &value);
        if (S_OK == r) {
            return patchBool(value);
        }
        break;
    }
    case STATUS_MAIN_HWND:
    case STATUS_TC_HWND:
    case STATUS_APP_HWND:
    case STATUS_PL_HWND:
    case STATUS_EQ_HWND: {
        msg = AIMP_MSG_PROPERTY_HWND;
        int param1 = 0;
        switch (status) {
        case STATUS_MAIN_HWND: param1 = AIMP_MPH_MAINFORM; break;
        case STATUS_APP_HWND:  param1 = AIMP_MPH_APPLICATION; break;
        case STATUS_TC_HWND:   param1 = AIMP_MPH_TRAYCONTROL; break;
        case STATUS_PL_HWND:   param1 = AIMP_MPH_PLAYLISTFORM; break;
        case STATUS_EQ_HWND:   param1 = AIMP_MPH_EQUALIZERFORM; break;
        default:
            assert(!"unknown status");
            break;
        }
        HWND value;
        r = aimp_service_message_dispatcher_->Send(msg, param1, &value);
        if (S_OK == r) {
            return reinterpret_cast<StatusValue>(value);
        }
        break;
    }
    case STATUS_TRAY: {
        msg = AIMP_MSG_PROPERTY_MINIMIZED_TO_TRAY;
        BOOL value;
        r = aimp_service_message_dispatcher_->Send(msg, param1, &value);
        if (S_OK == r) {
            return patchBool(value);
        }
        break;
    }
    default:
        break;
    }

    std::ostringstream os;
    os << "Failed to get status " << status << ". Reason: ";
    if (r != S_OK) {
        os << "IAIMPCoreUnit::MessageSend(" << msg << ", " << param1 << ") returned " << r << ".";
    } else {
        os << "status is not supported.";
    }
    throw std::runtime_error( os.str() );
}


void AIMPManager36::setStatus(AIMPManager::STATUS status, AIMPManager::StatusValue status_value)
{
    //try {
    //    if ( FALSE == aimp2_controller_->AIMP_Status_Set(cast<AIMP2SDK_STATUS>(status), value) ) {
    //        throw std::runtime_error(MakeString() << "Error occured while setting status " << asString(status) << " to value " << value);
    //    }
    //} catch (std::bad_cast& e) {
    //    throw std::runtime_error( e.what() );
    //}

    //notifyAboutInternalEventOnStatusChange(status);

    DWORD msg = 0;
    const int param1 = AIMP_MSG_PROPVALUE_SET;
    HRESULT r = S_OK;
    switch (status) {
    case STATUS_VOLUME: {
        msg = AIMP_MSG_PROPERTY_VOLUME;
        float value = status_value / 100.f;
        r = aimp_service_message_dispatcher_->Send(msg, param1, &value);
        if (S_OK == r) {
            return;
        }
        break;
    }
    case STATUS_BALANCE: {
        msg = AIMP_MSG_PROPERTY_BALANCE;
        float value = status_value / 50.f - 1.f;
        r = aimp_service_message_dispatcher_->Send(msg, param1, &value);
        if (S_OK == r) {
            return;
        }
        break;
    }
    case STATUS_SPEED: {
        msg = AIMP_MSG_PROPERTY_SPEED;
        float value = status_value / 100.f + 0.5f;
        r = aimp_service_message_dispatcher_->Send(msg, param1, &value);
        if (S_OK == r) {
            return;
        }
        break;
    }
    case STATUS_Player: {
        msg = AIMP_MSG_PROPERTY_PLAYER_STATE;
        int value = 0;
        switch (status_value) {
        case STOPPED: value = 0; break;
        case PAUSED:  value = 1; break;
        case PLAYING: value = 2; break;
        default:
            throw std::runtime_error( MakeString() << "Failed to set status: STATUS_Player. Reason: failed to convert status value " << status_value << " to AIMP3 value.");
        }
        r = aimp_service_message_dispatcher_->Send(msg, param1, &value);
        if (S_OK == r) {
            return;
        }
        break;
    }
    case STATUS_MUTE: {
        msg = AIMP_MSG_PROPERTY_MUTE;
        BOOL value = status_value;
        r = aimp_service_message_dispatcher_->Send(msg, param1, &value);
        if (S_OK == r) {
            return;
        }
        break;
    }
    case STATUS_REVERB: {
        msg = AIMP_MSG_PROPERTY_REVERB;
        float value = status_value / 100.f;
        r = aimp_service_message_dispatcher_->Send(msg, param1, &value);
        if (S_OK == r) {
            return;
        }
        break;
    }
    case STATUS_ECHO: {
        msg = AIMP_MSG_PROPERTY_ECHO;
        float value = status_value / 100.f;
        r = aimp_service_message_dispatcher_->Send(msg, param1, &value);
        if (S_OK == r) {
            return;
        }
        break;
    }
    case STATUS_CHORUS: {
        msg = AIMP_MSG_PROPERTY_CHORUS;
        float value = status_value / 100.f;
        r = aimp_service_message_dispatcher_->Send(msg, param1, &value);
        if (S_OK == r) {
            return;
        }
        break;
    }
    case STATUS_Flanger: {
        msg = AIMP_MSG_PROPERTY_FLANGER;
        float value = status_value / 100.f;
        r = aimp_service_message_dispatcher_->Send(msg, param1, &value);
        if (S_OK == r) {
            return;
        }
        break;
    }
    case STATUS_EQ_STS: {
        msg = AIMP_MSG_PROPERTY_EQUALIZER;
        BOOL value = status_value;
        r = aimp_service_message_dispatcher_->Send(msg, param1, &value);
        if (S_OK == r) {
            return;
        }
        break;
    }
    case STATUS_EQ_SLDR01:
    case STATUS_EQ_SLDR02:
    case STATUS_EQ_SLDR03:
    case STATUS_EQ_SLDR04:
    case STATUS_EQ_SLDR05:
    case STATUS_EQ_SLDR06:
    case STATUS_EQ_SLDR07:
    case STATUS_EQ_SLDR08:
    case STATUS_EQ_SLDR09:
    case STATUS_EQ_SLDR10:
    case STATUS_EQ_SLDR11:
    case STATUS_EQ_SLDR12:
    case STATUS_EQ_SLDR13:
    case STATUS_EQ_SLDR14:
    case STATUS_EQ_SLDR15:
    case STATUS_EQ_SLDR16:
    case STATUS_EQ_SLDR17:
    case STATUS_EQ_SLDR18: {
        msg = AIMP_MSG_PROPERTY_EQUALIZER_BAND;
        const int param1 = MAKELONG(AIMP_MSG_PROPVALUE_SET, status - STATUS_EQ_SLDR01);
        float value = status_value / 100.f * 30.f - 15.f;
        r = aimp_service_message_dispatcher_->Send(msg, param1, &value);
        if (S_OK == r) {
            return;
        }
        break;
    }
    case STATUS_REPEAT: {
        msg = AIMP_MSG_PROPERTY_REPEAT;
        BOOL value = status_value;
        r = aimp_service_message_dispatcher_->Send(msg, param1, &value);
        if (S_OK == r) {
            return;
        }
        break;
    }
    case STATUS_STAY_ON_TOP: {
        msg = AIMP_MSG_PROPERTY_STAYONTOP;
        BOOL value = status_value;
        r = aimp_service_message_dispatcher_->Send(msg, param1, &value);
        if (S_OK == r) {
            return;
        }
        break;                         
    }
    case STATUS_POS: {
        msg = AIMP_MSG_PROPERTY_PLAYER_POSITION;
        float value = static_cast<float>(status_value);
        r = aimp_service_message_dispatcher_->Send(msg, param1, &value);
        if (S_OK == r) {
            notifyAllExternalListeners(EVENT_TRACK_PROGRESS_CHANGED_DIRECTLY);
            return;
        }
        break;
    }
    case STATUS_LENGTH:
    case STATUS_KBPS:
    case STATUS_KHZ: {
        throw std::runtime_error( MakeString() << "Failed to set read-only status: " << asString(status) );
    }
    case STATUS_ACTION_ON_END_OF_PLAYLIST: {
        msg = AIMP_MSG_PROPERTY_ACTION_ON_END_OF_PLAYLIST;
        int value = status_value;
        r = aimp_service_message_dispatcher_->Send(msg, param1, &value);
        if (S_OK == r) {
            enum { JUMP_TO_THE_NEXT_PLAYLIST,
                   REPEAT_PLAYLIST,
                   DO_NOTHING };
            return;
        }
        break;
    }
    case STATUS_REPEAT_SINGLE_FILE_PLAYLISTS: {
        msg = AIMP_MSG_PROPERTY_REPEAT_SINGLE_FILE_PLAYLISTS;
        BOOL value = status_value;
        r = aimp_service_message_dispatcher_->Send(msg, param1, &value);
        if (S_OK == r) {
            return;
        }
        break;
    }
    //STATUS_MODE,
    //STATUS_STREAM_TYPE,
    case STATUS_REVERSETIME:
    case STATUS_SHUFFLE:
    case STATUS_RADIO_CAPTURE: {
        switch (status) {
        case STATUS_REVERSETIME:   msg = AIMP_MSG_PROPERTY_REVERSETIME; break;
        case STATUS_SHUFFLE:       msg = AIMP_MSG_PROPERTY_SHUFFLE;     break;
        case STATUS_RADIO_CAPTURE: msg = AIMP_MSG_PROPERTY_RADIOCAP;    break;
        default:
            assert(!"unknown status");
            break;
        }
        BOOL value = status_value;
        r = aimp_service_message_dispatcher_->Send(msg, param1, &value);
        if (S_OK == r) {
            return;
        }
        break;
    }
    case STATUS_MAIN_HWND:
    case STATUS_TC_HWND:
    case STATUS_APP_HWND:
    case STATUS_PL_HWND:
    case STATUS_EQ_HWND: {
        throw std::runtime_error( MakeString() << "Failed to set read-only status: STATUS_LENGTH");
        break;
    }
    case STATUS_TRAY: {
        msg = AIMP_MSG_PROPERTY_MINIMIZED_TO_TRAY;
        BOOL value = status_value;
        r = aimp_service_message_dispatcher_->Send(msg, param1, &value);
        if (S_OK == r) {
            return;
        }
        break;
    }
    default:
        break;
    }

    std::ostringstream os;
    os << "Failed to set status " << status << ". Reason: ";
    if (r != S_OK) {
        os << "IAIMPCoreUnit::MessageSend(" << msg << ", " << param1 << ") returned " << r << ".";
    } else {
        os << "status is not supported.";
    }
    throw std::runtime_error( os.str() );
}

void AIMPManager36::reloadQueuedEntries() // throws std::runtime_error
{
    // PROFILE_EXECUTION_TIME(__FUNCTION__);
/* ///!!! Impement after final implementation of loadEntries.

    deleteQueuedEntriesFromPlaylistDB(); // remove old entries before adding new ones.

    sqlite3_stmt* stmt = createStmt(playlists_db_, "INSERT INTO QueuedEntries VALUES (?,?,?,?,?,"
                                                                                     "?,?,?,?,?,"
                                                                                     "?,?,?,?,?)"
                                    );
    ON_BLOCK_EXIT(&sqlite3_finalize, stmt);

    AIMP3Util::FileInfoHelper file_info_helper; // used for get entries from AIMP conveniently.

    const int entries_count = aimp3_playlist_queue_->QueueEntryGetCount();
    for (int entry_index = 0; entry_index < entries_count; ++entry_index) {
        HPLSENTRY entry_handle;
        HRESULT r = aimp3_playlist_queue_->QueueEntryGet(entry_index, &entry_handle);

        if (S_OK != r) {
            const std::string msg = MakeString() << "IAIMPAddonsPlaylistQueue::QueueEntryGet() error " 
                                                 << r << " occured while getting entry info �" << entry_index;
            throw std::runtime_error(msg);
        }

        r = aimp3_playlist_manager_->EntryPropertyGetValue( entry_handle, AIMP_PLAYLIST_ENTRY_PROPERTY_INFO,
                                                            &file_info_helper.getEmptyFileInfo(), sizeof(file_info_helper.getEmptyFileInfo())
                                                            );

        if (S_OK != r) {
            const std::string msg = MakeString() << "IAIMPAddonsPlaylistManager::EntryPropertyGetValue() error " 
                                                 << r << " occured while getting entry info �" << entry_index;
            throw std::runtime_error(msg);
        }

        { // get rating manually, since AIMP3 does not fill TAIMPFileInfo::Rating value.
            int rating = 0;
            r = aimp3_playlist_manager_->EntryPropertyGetValue( entry_handle, AIMP3SDK::AIMP_PLAYLIST_ENTRY_PROPERTY_MARK, &rating, sizeof(rating) );    
            if (S_OK != r) {
                rating =  0;
            }

            // special db code
            {
#define bind(type, field_index, value)  rc_db = sqlite3_bind_##type(stmt, field_index, value); \
                                        if (SQLITE_OK != rc_db) { \
                                            const std::string msg = MakeString() << "Error sqlite3_bind_"#type << " " << rc_db; \
                                            throw std::runtime_error(msg); \
                                        }
#define bindText(field_index, info_field_name)  rc_db = sqlite3_bind_text16(stmt, field_index, info.##info_field_name##Buffer, info.##info_field_name##BufferSizeInChars * sizeof(WCHAR), SQLITE_STATIC); \
                                                if (SQLITE_OK != rc_db) { \
                                                    const std::string msg = MakeString() << "sqlite3_bind_text16" << " " << rc_db; \
                                                    throw std::runtime_error(msg); \
                                                }
                int rc_db;
                TrackDescription track_desc = getTrackDescOfQueuedEntry(entry_handle);
                bind(int,    1, track_desc.playlist_id);
                // bind all values
                const AIMP3SDK::TAIMPFileInfo& info = file_info_helper.getFileInfoWithCorrectStringLengthsAndNonEmptyTitle();
                bind(int,    2, track_desc.track_id);
                bind(int,    3, entry_index);
                bindText(    4, Album);
                bindText(    5, Artist);
                bindText(    6, Date);
                bindText(    7, FileName);
                bindText(    8, Genre);
                bindText(    9, Title);
                bind(int,   10, info.BitRate);
                bind(int,   11, info.Channels);
                bind(int,   12, info.Duration);
                bind(int64, 13, info.FileSize);
                bind(int,   14, rating);
                bind(int,   15, info.SampleRate);
                

                rc_db = sqlite3_step(stmt);
                if (SQLITE_DONE != rc_db) {
                    const std::string msg = MakeString() << "sqlite3_step() error "
                                                         << rc_db << ": " << sqlite3_errmsg(playlists_db_);
                    throw std::runtime_error(msg);
                }
                sqlite3_reset(stmt);
#undef bind
#undef bindText
            }
        }
    }
*/
}

TrackDescription AIMPManager36::getTrackDescOfQueuedEntry(AIMP36SDK::IAIMPPlaylistItem* item) const // throws std::runtime_error
{
    // We rely on fact that AIMP always have queued entry in one of playlists.

    const PlaylistEntryID entry_id = castToPlaylistEntryID(item);

    // find playlist id.
    const std::string& query = MakeString() << "SELECT playlist_id FROM PlaylistsEntries "
                                            << "WHERE entry_id = " << entry_id;

    sqlite3* playlists_db = playlists_db_;
    sqlite3_stmt* stmt = createStmt( playlists_db, query.c_str() );
    ON_BLOCK_EXIT(&sqlite3_finalize, stmt);

    for(;;) {
		int rc_db = sqlite3_step(stmt);
        if (SQLITE_ROW == rc_db) {
            int playlist_id = sqlite3_column_int(stmt, 0);
            return TrackDescription(playlist_id, entry_id);
        } else if (SQLITE_DONE == rc_db) {
            break;
        } else {
            const std::string msg = MakeString() << "sqlite3_step() error "
                                                 << rc_db << ": " << sqlite3_errmsg(playlists_db)
                                                 << ". Query: " << query;
            throw std::runtime_error(msg);
		}
    }

    throw std::runtime_error(MakeString() << "Queued entry is not found at existing playlists unexpectedly. Entry id: " << entry_id);
}

void AIMPManager36::moveQueueEntry(TrackDescription track_desc, int new_queue_index) // throws std::runtime_error
{
    TrackDescription absolute_track_desc(getAbsoluteTrackDesc(track_desc));
    if (IAIMPPlaylistItem_ptr item = getPlaylistItem(absolute_track_desc.track_id)) {
        HRESULT r = aimp_playlist_queue_->Move(item.get(), new_queue_index);
        if (S_OK != r) {
            throw std::runtime_error( MakeString() << __FUNCTION__": aimp_playlist_queue_->Move() failed for track " << track_desc << ". Result: " << r);
        }
    } else {
        throw std::runtime_error( MakeString() << __FUNCTION__": invalid track" << track_desc);
    }
}

void AIMPManager36::moveQueueEntry(int old_queue_index, int new_queue_index) // throws std::runtime_error
{
    HRESULT r = aimp_playlist_queue_->Move2(old_queue_index, new_queue_index);
    if (S_OK != r) {
        throw std::runtime_error( MakeString() << __FUNCTION__": aimp_playlist_queue_->Move() failed for old_queue_index " << old_queue_index << ", new_queue_index" << new_queue_index << ". Result: " << r);
    }
}

void AIMPManager36::enqueueEntryForPlay(TrackDescription track_desc, bool insert_at_queue_beginning) // throws std::runtime_error
{
    TrackDescription absolute_track_desc(getAbsoluteTrackDesc(track_desc));
    if (IAIMPPlaylistItem_ptr item = getPlaylistItem(absolute_track_desc.track_id)) {
        HRESULT r = aimp_playlist_queue_->Add(item.get(), insert_at_queue_beginning);
        if (S_OK != r) {
            throw std::runtime_error( MakeString() << __FUNCTION__": aimp_playlist_queue_->Add() failed for track " << track_desc << ". Result: " << r);
        }
    } else {
        throw std::runtime_error( MakeString() << __FUNCTION__": invalid track" << track_desc);
    }
}

void AIMPManager36::removeEntryFromPlayQueue(TrackDescription track_desc) // throws std::runtime_error
{
    TrackDescription absolute_track_desc(getAbsoluteTrackDesc(track_desc));
    if (IAIMPPlaylistItem_ptr item = getPlaylistItem(absolute_track_desc.track_id)) {
        HRESULT r = aimp_playlist_queue_->Delete(item.get());
        if (S_OK != r) {
            throw std::runtime_error( MakeString() << __FUNCTION__": aimp_playlist_queue_->Delete() failed for track " << track_desc << ". Result: " << r);
        }
    } else {
        throw std::runtime_error( MakeString() << __FUNCTION__": invalid track" << track_desc);
    }
}

PlaylistID AIMPManager36::getPlayingPlaylist() const
{
    IAIMPPlaylist* playlist_tmp;
    HRESULT r = aimp_service_playlist_manager_->GetPlayablePlaylist(&playlist_tmp);
    if (S_OK != r) {
        throw std::runtime_error(MakeString() << __FUNCTION__": aimp_service_playlist_manager_->GetPlayablePlaylist() failed. Result: " << r);
    }
    if (playlist_tmp) {
        playlist_tmp->Release();
    } else {
        // player is stopped at this time, return active playlist for compatibility with AIMPManager: AIMP2-AIMP3.5 returned active playlist in this case.
        r = aimp_service_playlist_manager_->GetActivePlaylist(&playlist_tmp);
        if (S_OK != r) {
            std::runtime_error(MakeString() << __FUNCTION__": aimp_service_playlist_manager_->GetActivePlaylist() failed. Result: " << r);
        }
        assert(playlist_tmp);
        playlist_tmp->Release();
    }
    return cast<PlaylistID>(playlist_tmp);
}

PlaylistEntryID AIMPManager36::getPlayingEntry() const
{    
    IAIMPPlaylistItem* playlist_item_tmp;
    HRESULT r = aimp_service_player_->GetPlaylistItem(&playlist_item_tmp);
    if (S_OK == r && playlist_item_tmp) {
        playlist_item_tmp->Release();
    } else {
        // player is stopped at this time, return active playlist for compatibility with AIMPManager: AIMP2-AIMP3.5 returned active entry from active playlist in this case.
        IAIMPPlaylist* playlist_tmp;
        r = aimp_service_playlist_manager_->GetActivePlaylist(&playlist_tmp);
        if (S_OK != r) {
            throw std::runtime_error(MakeString() << __FUNCTION__": aimp_service_playlist_manager_->GetActivePlaylist() failed. Result: " << r);
        }
        boost::intrusive_ptr<IAIMPPlaylist> playlist(playlist_tmp, false);

        if (playlist->GetItemCount() > 0) {
            r = playlist->GetItem(0, IID_IAIMPPlaylistItem, reinterpret_cast<void**>(&playlist_item_tmp));
            if (S_OK != r) {
                throw std::runtime_error(MakeString() << __FUNCTION__": playlist->GetItem(0, IID_IAIMPPlaylistItem) failed. Result: " << r);
            }
            playlist_item_tmp->Release();
        }
    }
    return castToPlaylistEntryID(playlist_item_tmp);
}

TrackDescription AIMPManager36::getPlayingTrack() const
{
	return TrackDescription( getPlayingPlaylist(), getPlayingEntry() );
}

PlaylistID AIMPManager36::getAbsolutePlaylistID(PlaylistID id) const
{
    if (id == -1) { // treat ID -1 as playing playlist.
        id = getPlayingPlaylist();
    }
    return id;
}

PlaylistEntryID AIMPManager36::getAbsoluteEntryID(PlaylistEntryID id) const
{
    if (id == -1) { // treat ID -1 as playing entry.
        id = getPlayingEntry();
    }

    return id;
}

TrackDescription AIMPManager36::getAbsoluteTrackDesc(TrackDescription track_desc) const
{
    return TrackDescription( getAbsolutePlaylistID(track_desc.playlist_id), getAbsoluteEntryID(track_desc.track_id) );
}

PlaylistCRC32& AIMPManager36::getPlaylistCRC32Object(PlaylistID playlist_id) const
{
    IAIMPPlaylist* playlist = cast<IAIMPPlaylist*>(playlist_id);
    auto it = std::find_if(playlist_helpers_.begin(), playlist_helpers_.end(), [playlist](const PlaylistHelper& h) { return h.playlist_.get() == playlist; });
    if (it != playlist_helpers_.end()) {
        return it->crc32_;
    }
    throw std::runtime_error(MakeString() << "Playlist " << playlist_id << " was not found in "__FUNCTION__);
}

crc32_t AIMPManager36::getPlaylistCRC32(PlaylistID playlist_id) const
{
    return getPlaylistCRC32Object(playlist_id).crc32();
}

void AIMPManager36::updatePlaylistCrcInDB(PlaylistID playlist_id, crc32_t crc32)
{
    sqlite3_stmt* stmt = createStmt(playlists_db_,
                                    "UPDATE Playlists SET crc32=? WHERE id=?"
                                    );
    ON_BLOCK_EXIT(&sqlite3_finalize, stmt);

#define bind(type, field_index, value)  rc_db = sqlite3_bind_##type(stmt, field_index, value); \
                                        if (SQLITE_OK != rc_db) { \
                                            const std::string msg = MakeString() << "Error sqlite3_bind_"#type << " " << rc_db; \
                                            throw std::runtime_error(msg); \
                                        }

    int rc_db;
    bind(int64, 1, crc32);
    bind(int,   2, playlist_id);
    
#undef bind
    rc_db = sqlite3_step(stmt);
    if (SQLITE_DONE != rc_db) {
        const std::string msg = MakeString() << "sqlite3_step() error "
                                             << rc_db << ": " << sqlite3_errmsg(playlists_db_);
        throw std::runtime_error(msg);
    }
}

AIMPManager::PLAYLIST_ENTRY_SOURCE_TYPE AIMPManager36::getTrackSourceType(TrackDescription track_desc) const
{
    const DWORD duration = getEntryField<DWORD>(playlists_db_, "duration", track_desc.track_id);
    return duration == 0 ? SOURCE_TYPE_RADIO : SOURCE_TYPE_FILE; // very shallow determination. Duration can be 0 on usual track if AIMP has no loaded track info yet.
}

AIMPManager::PLAYBACK_STATE AIMPManager36::getPlaybackState() const
{
    PLAYBACK_STATE state = STOPPED;
    // map internal AIMP state to PLAYBACK_STATE.
        // AParam1: 0 = Stopped; 1 = Paused; 2 = Playing
    int internal_state = aimp_service_player_->GetState();
    switch (internal_state) {
    case 0:
        state = STOPPED;
        break;
    case 1:
        state = PAUSED;
        break;
    case 2:
        state = PLAYING;
        break;
    default:
        assert(!"aimp_service_player_->GetState() returned unknown value");
        BOOST_LOG_SEV(logger(), error) << "aimp_service_player_->GetState() returned unknown value " << internal_state;
    }

    return state;
}

std::wstring AIMPManager36::getFormattedEntryTitle(TrackDescription track_desc, const std::string& format_string_utf8) const
{
    TrackDescription absolute_track_desc(getAbsoluteTrackDesc(track_desc));
    if (IAIMPPlaylistItem_ptr item = getPlaylistItem(absolute_track_desc.track_id)) {
        std::wstring wformat_string( StringEncoding::utf8_to_utf16(format_string_utf8) );

        { // use AIMP3 format specifiers.
        using namespace Utilities;
        // Aimp3 uses %R instead %a as Artist.
        replaceAll(L"%a", 2,
                   L"%R", 2,
                   &wformat_string);
        }

        IAIMPFileInfo* file_info_tmp;
        HRESULT r = item->GetValueAsObject(AIMP_PLAYLISTITEM_PROPID_FILEINFO, IID_IAIMPFileInfo,
                                           reinterpret_cast<void**>(&file_info_tmp)
                                           );
        if (S_OK != r) {
              throw std::runtime_error(MakeString() << __FUNCTION__": item->GetValueAsObject(AIMP_PLAYLISTITEM_PROPID_FILEINFO) for track " << absolute_track_desc << " failed. Result " << r);  
        }
        boost::intrusive_ptr<IAIMPFileInfo> file_info(file_info_tmp, false);
        file_info_tmp = nullptr;        

        IAIMPServiceFileInfoFormatter* formatter_tmp;
        r = aimp36_core_->QueryInterface(IID_IAIMPServiceFileInfoFormatter,
                                         reinterpret_cast<void**>(&formatter_tmp)
                                         );
        if (S_OK != r) {
              throw std::runtime_error(MakeString() << __FUNCTION__": aimp36_core_->QueryInterface(IID_IAIMPServiceFileInfoFormatter) for track " << absolute_track_desc << " failed. Result " << r);  
        }
        boost::intrusive_ptr<IAIMPServiceFileInfoFormatter> formatter(formatter_tmp, false);
        formatter_tmp = nullptr;   
    
        // HRESULT WINAPI Format(IAIMPString *Template, IAIMPFileInfo *FileInfo, int Reserved, IUnknown *AdditionalInfo, IAIMPString **FormattedResult) = 0;
        AIMPString template_string(&wformat_string, false);
        template_string.AddRef(); // prevent destruction by AIMP.
        IAIMPString* formatted_string_tmp;
        r = formatter->Format(&template_string,
                              file_info.get(),
                              0,
                              nullptr,
                              &formatted_string_tmp
                              );
        if (S_OK != r) {
            throw std::runtime_error(MakeString() << __FUNCTION__": formatter->Format() failed. Result: " << r << ". format_string_utf8: " << format_string_utf8);
        }
        boost::intrusive_ptr<IAIMPString> formatted_string(formatted_string_tmp, false);
        return std::wstring(formatted_string->GetData(), formatted_string->GetLength());    
    } else {
        throw std::runtime_error( MakeString() << __FUNCTION__": invalid track" << track_desc);
    }
}

std::wstring AIMPManager36::getEntryFilename(TrackDescription /*track_desc*/) const
{
	BOOST_LOG_SEV(logger(), debug) << "AIMPManager36::getEntryFilename"; ///!!! TODO: implement
    return std::wstring();
}

bool AIMPManager36::isCoverImageFileExist(TrackDescription /*track_desc*/, boost::filesystem::wpath* /*path*/) const
{
	BOOST_LOG_SEV(logger(), debug) << "AIMPManager36::isCoverImageFileExist"; ///!!! TODO: implement
    return false;
}

void AIMPManager36::saveCoverToFile(TrackDescription /*track_desc*/, const std::wstring& /*filename*/, int /*cover_width*/, int /*cover_height*/ ) const
{
	BOOST_LOG_SEV(logger(), debug) << "AIMPManager36::saveCoverToFile"; ///!!! TODO: implement
}

int AIMPManager36::trackRating(TrackDescription /*track_desc*/) const
{
	BOOST_LOG_SEV(logger(), debug) << "AIMPManager36::trackRating"; ///!!! TODO: implement
    return 0;
}

void AIMPManager36::addFileToPlaylist(const boost::filesystem::wpath& /*path*/, PlaylistID /*playlist_id*/)
{
	BOOST_LOG_SEV(logger(), debug) << "AIMPManager36::addFileToPlaylist"; ///!!! TODO: implement
}
    
void AIMPManager36::addURLToPlaylist(const std::string& /*url*/, PlaylistID /*playlist_id*/)
{
	BOOST_LOG_SEV(logger(), debug) << "AIMPManager36::addURLToPlaylist"; ///!!! TODO: implement
}

PlaylistID AIMPManager36::createPlaylist(const std::wstring& /*title*/)
{
	BOOST_LOG_SEV(logger(), debug) << "AIMPManager36::createPlaylist"; ///!!! TODO: implement
    return PlaylistID();
}

void AIMPManager36::removeTrack(TrackDescription /*track_desc*/, bool /*physically*/)
{
	BOOST_LOG_SEV(logger(), debug) << "AIMPManager36::removeTrack"; ///!!! TODO: implement
}

void AIMPManager36::onTick()
{
}

std::string playlist36NotifyFlagsToString(DWORD flags)
{
#ifndef NDEBUG
    const DWORD all_flags =   AIMP_PLAYLIST_NOTIFY_NAME     | AIMP_PLAYLIST_NOTIFY_SELECTION  | AIMP_PLAYLIST_NOTIFY_PLAYBACKCURSOR
                            | AIMP_PLAYLIST_NOTIFY_READONLY | AIMP_PLAYLIST_NOTIFY_FOCUSINDEX | AIMP_PLAYLIST_NOTIFY_CONTENT
                            | AIMP_PLAYLIST_NOTIFY_FILEINFO | AIMP_PLAYLIST_NOTIFY_STATISTICS | AIMP_PLAYLIST_NOTIFY_PLAYINGSWITCHS
                            | AIMP_PLAYLIST_NOTIFY_PREIMAGE;
    assert(flags <= all_flags);
#endif

    const DWORD flag_count = 10;
    static const char * const strings[flag_count] = { "NAME",     "SELECTION",  "PLAYBACKCURSOR",
                                                      "READONLY", "FOCUSINDEX", "CONTENT",
                                                      "FILEINFO", "STATISTICS", "PLAYINGSWITCHS",
                                                      "PREIMAGE"
                                                     };
    std::ostringstream os;
    if (flags) {
        for (DWORD f = flags, index = 0; f != 0; ++index, f >>= 1) {
            if ( (f & 0x1) == 0x1 ) {
                if ( os.tellp() != std::streampos(0) ) {
                    os << '|';
                }
                if (index < flag_count) {
                    os << strings[index];
                } else {
                    os << "out of range value";
                }
            }
        }
    } else {
        os << "NONE";
    }
    return os.str();
}

} // namespace AIMPPlayer