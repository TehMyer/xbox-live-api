//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************
#include "pch.h"
#include "xsapi/stats_manager.h"
#include "stats_manager_internal.h"
#include "xsapi/services.h"
#include "xsapi/system.h"
#include "xbox_live_context_impl.h"
#include "ppltasks_extra.h"

using namespace xbox::services;
using namespace xbox::services::system;

NAMESPACE_MICROSOFT_XBOX_SERVICES_STAT_MANAGER_CPP_BEGIN

const std::chrono::seconds stats_manager_impl::TIME_PER_CALL_SEC =
#if UNIT_TEST_SERVICES
std::chrono::seconds::zero();
#else
std::chrono::seconds(60);
#endif

const std::chrono::milliseconds stats_manager_impl::STATS_POLL_TIME_MS = std::chrono::milliseconds(16);

stats_manager_impl::stats_manager_impl()
{
}

void
stats_manager_impl::initialize()
{
    std::weak_ptr<stats_manager_impl> thisWeakPtr = shared_from_this();
    m_statTimer = std::make_shared<call_buffer_timer>(
    [thisWeakPtr](std::vector<string_t> eventArgs, const call_buffer_timer_completion_context&)
    {
        std::shared_ptr<stats_manager_impl> pThis(thisWeakPtr.lock());
        if (pThis != nullptr && !eventArgs.empty())
        {
            pThis->request_flush_to_service_callback(eventArgs[0]);
        }
    },
    TIME_PER_CALL_SEC
    );

    m_statPriorityTimer = std::make_shared<call_buffer_timer>(
    [thisWeakPtr](std::vector<string_t> eventArgs, const call_buffer_timer_completion_context&)
    {
        std::shared_ptr<stats_manager_impl> pThis(thisWeakPtr.lock());
        if (pThis != nullptr && !eventArgs.empty())
        {
            pThis->request_flush_to_service_callback(eventArgs[0]);
        }
    },
    TIME_PER_CALL_SEC
    );

    run_flush_timer();
}

void
stats_manager_impl::run_flush_timer()
{
    std::weak_ptr<stats_manager_impl> thisWeakPtr = shared_from_this();
    pplx::extras::create_delayed_task(
        STATS_POLL_TIME_MS,
        [thisWeakPtr]()
    {
        std::shared_ptr<stats_manager_impl> pThis(thisWeakPtr.lock());
        if (pThis != nullptr)
        {
            std::lock_guard<std::mutex> guard(pThis->m_statsServiceMutex);
            for (auto& user : pThis->m_users)
            {
                if (user.second.statValueDocument.is_dirty())
                {
                    pThis->flush_to_service(user.second);
                    user.second.statValueDocument.clear_dirty_state();
                }
            }
            pThis->run_flush_timer();
        }
    });
}

xbox_live_result<void>
stats_manager_impl::add_local_user(
    _In_ const xbox_live_user_t& user
    )
{
    std::lock_guard<std::mutex> guard(m_statsServiceMutex);
    string_t userStr = user_context::get_user_id(user);
    auto userIter = m_users.find(userStr);
    if (userIter != m_users.end())
    {
        return xbox_live_result<void>(xbox_live_error_code::invalid_argument, "User already in local map");
    }

    auto xboxLiveContextImpl = std::make_shared<xbox_live_context_impl>(user);
    xboxLiveContextImpl->user_context()->set_caller_context_type(caller_context_type::stats_manager);
    xboxLiveContextImpl->init();
    auto simplifiedStatsService = simplified_stats_service(
        xboxLiveContextImpl->user_context(),
        xboxLiveContextImpl->settings(),
        xboxLiveContextImpl->application_config()
        );

    m_users[userStr] = stats_user_context(stats_value_document(), xboxLiveContextImpl, simplifiedStatsService, user);
    std::weak_ptr<stats_manager_impl> thisWeak = shared_from_this();
    simplifiedStatsService.get_stats_value_document()
    .then([thisWeak, user, xboxLiveContextImpl, simplifiedStatsService, userStr](xbox_live_result<stats_value_document> statsValueDocResult)
    {
        std::shared_ptr<stats_manager_impl> pThis(thisWeak.lock());
        if (pThis == nullptr)
        {
            return;
        }

        std::lock_guard<std::mutex> guard(pThis->m_statsServiceMutex);

        bool isSignedIn = false;
#if TV_API
        isSignedIn = user->IsSignedIn;
#else
        isSignedIn = user->is_signed_in();
#endif

        if (isSignedIn)
        {
            auto& svd = statsValueDocResult.payload();
            auto userStatContext = pThis->m_users.find(userStr);
            if (userStatContext != pThis->m_users.end())    // user could be removed by the time this completes
            {
                if (statsValueDocResult.err())  // if there was an error, but the user is signed in, we assume offline sign in
                {
                    userStatContext->second.statValueDocument.set_state(svd_state::offline_not_loaded);
                }
                else
                {
                    pThis->m_users[userStr].statValueDocument.merge_stat_value_documents(svd);
                }
            }

            pThis->m_users[userStr].statValueDocument.set_flush_function([thisWeak, userStr, user]()
            {
                std::shared_ptr<stats_manager_impl> pThis(thisWeak.lock());
                if (pThis == nullptr)
                {
                    return;
                }
                auto statContextIter = pThis->m_users.find(userStr);
                if (statContextIter == pThis->m_users.end())
                {
                    return;
                }

                pThis->flush_to_service(
                    statContextIter->second
                    );
            });
        }
        else    // not offline signed in
        {
            LOG_DEBUG("Could not successfully get SVD for user and user is offline");
        }

        pThis->m_statEventList.push_back(stat_event(stat_event_type::local_user_added, user, xbox_live_result<void>(statsValueDocResult.err(), statsValueDocResult.err_message())));
    });

    return xbox_live_result<void>();
}

xbox_live_result<void>
stats_manager_impl::remove_local_user(
    _In_ const xbox_live_user_t& user
)
{
    std::lock_guard<std::mutex> guard(m_statsServiceMutex);
    string_t userStr = user_context::get_user_id(user);
    auto userIter = m_users.find(userStr);
    if (userIter == m_users.end())
    {
        return xbox_live_result<void>(xbox_live_error_code::invalid_argument, "User not found in local map");
    }

    auto statsUserContext = userIter->second;
    auto userSVD = statsUserContext.statValueDocument;
    if (userSVD.is_dirty())
    {
        userSVD.do_work();  // before removing the user apply all users
        std::weak_ptr<stats_manager_impl> thisWeak = shared_from_this();
        userIter->second.simplifiedStatsService.update_stats_value_document(userSVD)
        .then([thisWeak, userSVD, user, userStr](xbox_live_result<void> updateSVDResult)
        {
            std::shared_ptr<stats_manager_impl> pThis(thisWeak.lock());
            if (pThis == nullptr)
            {
                return;
            }

            std::lock_guard<std::mutex> guard(pThis->m_statsServiceMutex);
            auto statsUserContextIter = pThis->m_users.find(userStr);
            if (statsUserContextIter == pThis->m_users.end())
            {
                return;
            }

            if(should_write_offline(updateSVDResult))
            {
                pThis->write_offline(statsUserContextIter->second);
            }

            pThis->m_statEventList.push_back(stat_event(stat_event_type::local_user_removed, user, updateSVDResult));
            pThis->m_users.erase(userStr);
        });
    }
    else
    {
        m_statEventList.push_back(stat_event(stat_event_type::local_user_removed, user, xbox_live_result<void>()));
        m_users.erase(userIter);
    }

    return xbox_live_result<void>();
}

xbox_live_result<void>
stats_manager_impl::request_flush_to_service(
    _In_ const xbox_live_user_t& user,
    _In_ bool isHighPriority
    )
{
    std::lock_guard<std::mutex> guard(m_statsServiceMutex);
    string_t userStr = user_context::get_user_id(user);
    auto userIter = m_users.find(userStr);
    if (userIter == m_users.end())
    {
        return xbox_live_result<void>(xbox_live_error_code::invalid_argument, "User not found in local map");
    }

    auto& userSVD = userIter->second.statValueDocument;
    if (userSVD.is_dirty())
    {
        userSVD.do_work();
        userSVD.clear_dirty_state();
    }

    std::vector<string_t> userVec;
    userVec.push_back(userStr);

    if (isHighPriority)
    {
        m_statPriorityTimer->fire(userVec);
    }
    else
    {
        m_statTimer->fire(userVec);
    }

    return xbox_live_result<void>();
}

void
stats_manager_impl::flush_to_service(
    _In_ stats_user_context& statsUserContext
)
{
    std::weak_ptr<stats_manager_impl> thisWeak = shared_from_this();
    auto userStr = user_context::get_user_id(statsUserContext.xboxLiveUser);
    auto& svd = statsUserContext.statValueDocument;
    if (svd.state() != svd_state::loaded)   // if not loaded, try and get the SVD from the service
    {
        statsUserContext.simplifiedStatsService.get_stats_value_document().then([thisWeak, userStr](xbox_live_result<stats_value_document> svdResult)
        {
            std::shared_ptr<stats_manager_impl> pThis(thisWeak.lock());
            if (pThis == nullptr)
            {
                return;
            }

            if (!svdResult.err())
            {
                pThis->m_users[userStr].statValueDocument.merge_stat_value_documents(svdResult.payload());
            }

            pThis->update_stats_value_document(pThis->m_users[userStr]);
        });
    }
    else
    {
        update_stats_value_document(statsUserContext);
    }

}
void
stats_manager_impl::update_stats_value_document(_In_ stats_user_context& statsUserContext)
{
    std::weak_ptr<stats_manager_impl> thisWeak = shared_from_this();
    xbox_live_user_t user = statsUserContext.xboxLiveUser;
    auto userStr = user_context::get_user_id(user);

    statsUserContext.simplifiedStatsService.update_stats_value_document(statsUserContext.statValueDocument)
    .then([thisWeak, user, userStr](xbox_live_result<void> updateSVDResult)
    {
        std::shared_ptr<stats_manager_impl> pThis(thisWeak.lock());
        if (pThis == nullptr)
        {
            return;
        }
        std::lock_guard<std::mutex> guard(pThis->m_statsServiceMutex);

        auto statsUserContextIter = pThis->m_users.find(userStr);
        if (statsUserContextIter == pThis->m_users.end())
        {
            return;
        }

        auto& statsUserContext = statsUserContextIter->second;
        if (updateSVDResult.err())
        {
            if (should_write_offline(updateSVDResult))
            {
                if (statsUserContext.statValueDocument.state() == svd_state::loaded)
                {
                    statsUserContext.statValueDocument.set_state(svd_state::offline_loaded);
                }

                pThis->write_offline(statsUserContext);
            }
            else
            {
                LOG_ERROR("Stats manager could not write stats value document");
            }
        }

        pThis->m_statEventList.push_back(stat_event(stat_event_type::stat_update_complete, user, updateSVDResult));
    });
}

void
stats_manager_impl::request_flush_to_service_callback(
    _In_ const string_t& userXuid
    )
{
    auto userIter = m_users.find(userXuid);
    if (userIter != m_users.end())
    {
        flush_to_service(
            userIter->second
            );
    }
}

std::vector<stat_event>
stats_manager_impl::do_work()
{
    std::lock_guard<std::mutex> guard(m_statsServiceMutex);
    auto copyList = m_statEventList;
    for (auto& statUserContext : m_users)
    {
        statUserContext.second.statValueDocument.do_work();
    }
    m_statEventList.clear();
    return copyList;
}

xbox_live_result<void>
stats_manager_impl::set_stat(
    _In_ const xbox_live_user_t& user,
    _In_ const string_t& name,
    _In_ double value
    )
{
    std::lock_guard<std::mutex> guard(m_statsServiceMutex);
    string_t userStr = user_context::get_user_id(user);
    auto userIter = m_users.find(userStr);
    if (userIter == m_users.end())
    {
        return xbox_live_result<void>(xbox_live_error_code::invalid_argument, "User not found in local map");
    }

    return userIter->second.statValueDocument.set_stat(name.c_str(), value);
}

xbox_live_result<void>
stats_manager_impl::set_stat(
    _In_ const xbox_live_user_t& user,
    _In_ const string_t& name,
    _In_ const char_t* value
)
{
    std::lock_guard<std::mutex> guard(m_statsServiceMutex);
    string_t userStr = user_context::get_user_id(user);
    auto userIter = m_users.find(userStr);
    if (userIter == m_users.end())
    {
        return xbox_live_result<void>(xbox_live_error_code::invalid_argument, "User not found in local map");
    }

    return userIter->second.statValueDocument.set_stat(name.c_str(), value);
}

xbox_live_result<std::shared_ptr<stat_value>>
stats_manager_impl::get_stat(
    _In_ const xbox_live_user_t& user,
    _In_ const string_t& name
    )
{
    std::lock_guard<std::mutex> guard(m_statsServiceMutex);
    string_t userStr = user_context::get_user_id(user);
    auto userIter = m_users.find(userStr);
    if (userIter == m_users.end())
    {
        return xbox_live_result<std::shared_ptr<stat_value>>(xbox_live_error_code::invalid_argument, "User not found in local map");
    }

    return userIter->second.statValueDocument.get_stat(name.c_str());
}

xbox_live_result<void>
stats_manager_impl::get_stat_names(
    _In_ const xbox_live_user_t& user,
    _Inout_ std::vector<string_t>& statNameList
    )
{
    std::lock_guard<std::mutex> guard(m_statsServiceMutex);
    string_t userStr = user_context::get_user_id(user);
    auto userIter = m_users.find(userStr);
    if (userIter == m_users.end())
    {
        return xbox_live_result<void>(xbox_live_error_code::invalid_argument, "User not found in local map");
    }

    userIter->second.statValueDocument.get_stat_names(statNameList);

    return xbox_live_result<void>();
}

#if TV_API
void
stats_manager_impl::write_offline(
    _In_ const stats_user_context& userContext
    )
{
    UNREFERENCED_PARAMETER(userContext);
    // TODO: implement
}

#elif !UNIT_TEST_SERVICES
void
stats_manager_impl::write_offline(
    _In_ const stats_user_context& userContext
    )
{
    web::json::value evtJson;
    evtJson[_T("svd")] = userContext.statValueDocument.serialize();
    auto result = userContext.xboxLiveContextImpl->events_service().write_in_game_event(_T("StatEvent"), evtJson, web::json::value());
    if (result.err())
    {
        LOG_ERROR("Offline write for stats failed");
    }
}
#else
void
stats_manager_impl::write_offline(
    _In_ const stats_user_context& userContext
)
{
    UNREFERENCED_PARAMETER(userContext);
}
#endif

NAMESPACE_MICROSOFT_XBOX_SERVICES_STAT_MANAGER_CPP_END