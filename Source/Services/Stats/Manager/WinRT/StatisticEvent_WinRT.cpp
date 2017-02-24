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
#include "user_context.h"
#include "StatisticEvent_WinRT.h"

using namespace xbox::services;

NAMESPACE_MICROSOFT_XBOX_SERVICES_STATISTIC_MANAGER_BEGIN

StatisticEvent::StatisticEvent(
    _In_ xbox::services::stats::manager::stat_event cppObj
    ) :
    m_cppObj(std::move(cppObj))
{
    m_user = user_context::user_convert(m_cppObj.local_user());
    if (m_cppObj.error_info().err())
    {
        m_errorCode = ref new Platform::Exception(xbox::services::utils::convert_xbox_live_error_code_to_hresult(m_cppObj.error_info().err()));
    }

    auto message = m_cppObj.error_info().err_message();
    m_errorMessage = ref new Platform::String(utility::conversions::utf8_to_utf16(message).c_str());
}

XboxLiveUser_t
StatisticEvent::User::get()
{
    return m_user;
}

int
StatisticEvent::ErrorCode::get()
{
    if (m_errorCode == nullptr)
    {
        return 0;
    }

    return m_errorCode->HResult;
}

Platform::String^
StatisticEvent::ErrorMessage::get()
{
    return m_errorMessage;
}

NAMESPACE_MICROSOFT_XBOX_SERVICES_STATISTIC_MANAGER_END