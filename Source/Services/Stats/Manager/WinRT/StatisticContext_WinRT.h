//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************
#pragma once

NAMESPACE_MICROSOFT_XBOX_SERVICES_STATISTIC_MANAGER_BEGIN

public ref class StatisticContext sealed
{
public:
    StatisticContext(
        _In_ Platform::String^ name,
        _In_ Platform::String^ value
        );

    DEFINE_PROP_GET_STR_OBJ(Name, name);
    DEFINE_PROP_GET_STR_OBJ(Value, value);

internal:
    StatisticContext(
        _In_ xbox::services::experimental::stats::manager::stat_context cppObj
        );

    const xbox::services::experimental::stats::manager::stat_context& GetCppObj() const;

private:
    xbox::services::experimental::stats::manager::stat_context m_cppObj;
};

NAMESPACE_MICROSOFT_XBOX_SERVICES_STATISTIC_MANAGER_END