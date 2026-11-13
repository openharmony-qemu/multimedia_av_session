/*
 * Copyright (c) 2022-2025 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "avsession_errors.h"
#include "avsession_trace.h"
#include "napi_avsession_manager.h"
#include "napi_avcast_picker_helper.h"

namespace OHOS::AVSession {

static const std::string ABILITY_WANT_PARAMS_UIEXTENSIONTARGETTYPE = "ability.want.params.uiExtensionType";
static const std::string ABILITY_WANT_ELEMENT_NAME = "com.hmos.mediacontroller";

static __thread napi_ref AVCastPickerHelperConstructorRef = nullptr;
std::map<std::string, std::pair<NapiAVCastPickerHelper::OnEventHandlerType,
    NapiAVCastPickerHelper::OffEventHandlerType>> NapiAVCastPickerHelper::eventHandlers_ = {
    { "pickerStateChange", { OnPickerStateChange, OffPickerStateChange } },
};

NapiAVCastPickerHelper::NapiAVCastPickerHelper(Ace::UIContent* uiContent)
{
    SLOGI("construct");
    SLOGI("AVPicker lifetime helper=%{public}p phase=construct", static_cast<void*>(this));
    uiContent_ = uiContent;
    isValid_ = std::make_shared<bool>(true);
    audioRoutingMngr_ = AudioStandard::AudioRoutingManager::GetInstance();
}

NapiAVCastPickerHelper::~NapiAVCastPickerHelper()
{
    SLOGI("destroy.");
    *isValid_ = false;
    SLOGI("AVPicker lifetime helper=%{public}p phase=destroy", static_cast<void*>(this));
}

napi_value NapiAVCastPickerHelper::Init(napi_env env, napi_value exports)
{
    napi_property_descriptor descriptors[] = {
        DECLARE_NAPI_FUNCTION("select", SelectAVPicker),
        DECLARE_NAPI_FUNCTION("resetCommunicationDevice", ResetCommunicationDevice),
        DECLARE_NAPI_FUNCTION("on", OnEvent),
        DECLARE_NAPI_FUNCTION("off", OffEvent),
    };
    auto propertyCount = sizeof(descriptors) / sizeof(napi_property_descriptor);
    napi_value constructor {};
    auto status = napi_define_class(env, "AVCastPickerHelper", NAPI_AUTO_LENGTH, ConstructorCallback, nullptr,
                                    propertyCount, descriptors, &constructor);
    if (status != napi_ok) {
        SLOGE("define class failed");
        return NapiUtils::GetUndefinedValue(env);
    }
    napi_create_reference(env, constructor, 1, &AVCastPickerHelperConstructorRef);
    napi_set_named_property(env, exports, "AVCastPickerHelper", constructor);

    return exports;
}

napi_value NapiAVCastPickerHelper::ConstructorCallback(napi_env env, napi_callback_info info)
{
    struct ConcreteContext : public ContextBase { Ace::UIContent* uiContent; };
    auto context = std::make_shared<ConcreteContext>();
    if (context == nullptr) {
        NapiUtils::ThrowError(env, "ConstructorCallback failed : no memory",
            NapiAVSessionManager::errcode_[ERR_NO_MEMORY]);
        return NapiUtils::GetUndefinedValue(env);
    }
    auto inputParser = [env, context](size_t argc, napi_value* argv) {
        CHECK_ARGS_RETURN_VOID(context, argc == ARGC_ONE, "invalid arguments",
            NapiAVSessionManager::errcode_[ERR_INVALID_PARAM]);
        auto stageCtx = AbilityRuntime::GetStageModeContext(env, argv[ARGV_FIRST]);
        if (stageCtx == nullptr) {
            context->status = napi_generic_failure;
            NapiUtils::ThrowError(env, "get stageCtx failed", NapiAVSessionManager::errcode_[ERR_INVALID_PARAM]);
            return;
        }
        auto abilityContext = AbilityRuntime::Context::ConvertTo<AbilityRuntime::AbilityContext>(stageCtx);
        if (abilityContext != nullptr) {
            context->uiContent = abilityContext->GetUIContent();
        } else {
            auto extensionContext = AbilityRuntime::Context::ConvertTo<AbilityRuntime::UIExtensionContext>(stageCtx);
            CHECK_RETURN_VOID(extensionContext != nullptr, "convert to AbilityContext and ExtensionContext fail");
            context->uiContent = extensionContext->GetUIContent();
        }
        context->status = napi_ok;
    };
    context->GetCbInfo(env, info, inputParser);
    CHECK_AND_RETURN_RET_LOG(context->status == napi_ok, nullptr, "no need to wrap");

    auto* napiAVCastPickerHelper = new(std::nothrow) NapiAVCastPickerHelper(context->uiContent);
    CHECK_AND_RETURN_RET_LOG(napiAVCastPickerHelper != nullptr, nullptr, "no memory");
    auto finalize = [](napi_env env, void* data, void* hint) {
        auto* napiAVCastPickerHelper = reinterpret_cast<NapiAVCastPickerHelper*>(data);
        CHECK_AND_RETURN_LOG(napiAVCastPickerHelper != nullptr, "napiAVCastPickerHelper is nullptr");
        napi_delete_reference(env, napiAVCastPickerHelper->wrapperRef_);
        delete napiAVCastPickerHelper;
        napiAVCastPickerHelper = nullptr;
    };
    if (napi_wrap(env, context->self, static_cast<void*>(napiAVCastPickerHelper),
        finalize, nullptr, nullptr) != napi_ok) {
        SLOGE("wrap failed");
        delete napiAVCastPickerHelper;
        napiAVCastPickerHelper = nullptr;
        return nullptr;
    }
    return context->self;
}

napi_value NapiAVCastPickerHelper::OnEvent(napi_env env, napi_callback_info info)
{
    auto context = std::make_shared<ContextBase>();
    if (context == nullptr) {
        SLOGE("OnEvent failed : no memory");
        NapiUtils::ThrowError(env, "OnEvent failed : no memory", NapiAVSessionManager::errcode_[ERR_NO_MEMORY]);
        return NapiUtils::GetUndefinedValue(env);
    }

    std::string eventName;
    napi_value callback {};
    auto input = [&eventName, &callback, env, &context](size_t argc, napi_value* argv) {
        CHECK_ARGS_RETURN_VOID(context, argc == ARGC_TWO, "invalid argument number",
            NapiAVSessionManager::errcode_[ERR_INVALID_PARAM]);
        context->status = NapiUtils::GetValue(env, argv[ARGV_FIRST], eventName);
        CHECK_STATUS_RETURN_VOID(context, "get event name failed", NapiAVSessionManager::errcode_[ERR_INVALID_PARAM]);
        napi_valuetype type = napi_undefined;
        context->status = napi_typeof(env, argv[ARGV_SECOND], &type);
        CHECK_ARGS_RETURN_VOID(context, (context->status == napi_ok) && (type == napi_function),
                               "callback type invalid", NapiAVSessionManager::errcode_[ERR_INVALID_PARAM]);
        callback = argv[ARGV_SECOND];
    };
    context->GetCbInfo(env, info, input, true);
    if (context->status != napi_ok) {
        NapiUtils::ThrowError(env, context->errMessage.c_str(), context->errCode);
        return NapiUtils::GetUndefinedValue(env);
    }
    auto it = eventHandlers_.find(eventName);
    if (it == eventHandlers_.end()) {
        SLOGE("event name invalid");
        NapiUtils::ThrowError(env, "event name invalid", NapiAVSessionManager::errcode_[ERR_INVALID_PARAM]);
        return NapiUtils::GetUndefinedValue(env);
    }
    auto* napiAVCastPickerHelper = reinterpret_cast<NapiAVCastPickerHelper*>(context->native);
    if (it->second.first(env, napiAVCastPickerHelper, callback) != napi_ok) {
        NapiUtils::ThrowError(env, "add event callback failed", NapiAVSessionManager::errcode_[ERR_INVALID_PARAM]);
    }
    return NapiUtils::GetUndefinedValue(env);
}

napi_value NapiAVCastPickerHelper::OffEvent(napi_env env, napi_callback_info info)
{
    auto context = std::make_shared<ContextBase>();
    if (context == nullptr) {
        SLOGE("OffEvent failed : no memory");
        NapiUtils::ThrowError(env, "OffEvent failed : no memory", NapiAVSessionManager::errcode_[ERR_NO_MEMORY]);
        return NapiUtils::GetUndefinedValue(env);
    }
    std::string eventName;
    napi_value callback = nullptr;
    auto input = [&eventName, env, &context, &callback](size_t argc, napi_value* argv) {
        CHECK_ARGS_RETURN_VOID(context, argc == ARGC_ONE || argc == ARGC_TWO,
                               "invalid argument number", NapiAVSessionManager::errcode_[ERR_INVALID_PARAM]);
        context->status = NapiUtils::GetValue(env, argv[ARGV_FIRST], eventName);
        CHECK_STATUS_RETURN_VOID(context, "get event name failed", NapiAVSessionManager::errcode_[ERR_INVALID_PARAM]);
        if (argc == ARGC_TWO) {
            callback = argv[ARGV_SECOND];
        }
    };
    context->GetCbInfo(env, info, input, true);
    if (context->status != napi_ok) {
        NapiUtils::ThrowError(env, context->errMessage.c_str(), context->errCode);
        return NapiUtils::GetUndefinedValue(env);
    }
    auto it = eventHandlers_.find(eventName);
    if (it == eventHandlers_.end()) {
        SLOGE("event name invalid");
        NapiUtils::ThrowError(env, "event name invalid", NapiAVSessionManager::errcode_[ERR_INVALID_PARAM]);
        return NapiUtils::GetUndefinedValue(env);
    }
    auto* napiAVCastPickerHelper = reinterpret_cast<NapiAVCastPickerHelper*>(context->native);
    if (napiAVCastPickerHelper == nullptr) {
        SLOGE("OffEvent failed : napiAVCastPickerHelper is nullptr");
        NapiUtils::ThrowError(env, "OffEvent failed : napiAVCastPickerHelper is nullptr",
            NapiAVSessionManager::errcode_[ERR_INVALID_PARAM]);
        return NapiUtils::GetUndefinedValue(env);
    }
    if (napiAVCastPickerHelper != nullptr && it->second.second(env, napiAVCastPickerHelper, callback) != napi_ok) {
        NapiUtils::ThrowError(env, "remove event callback failed", NapiAVSessionManager::errcode_[ERR_INVALID_PARAM]);
    }
    return NapiUtils::GetUndefinedValue(env);
}

napi_value NapiAVCastPickerHelper::SelectAVPicker(napi_env env, napi_callback_info info)
{
    auto context = std::make_shared<ConcreteContext>();
    auto inputParser = [env, context](size_t argc, napi_value* argv) {
        CHECK_ARGS_RETURN_VOID(context, argc == ARGC_ZERO || argc == ARGC_ONE,
                               "invalid argument number", NapiAVSessionManager::errcode_[ERR_INVALID_PARAM]);
        if (argc == ARGC_ONE) {
            context->status = NapiUtils::GetValue(env, argv[ARGV_FIRST], context->napiAVCastPickerOptions);
        }
        CHECK_STATUS_RETURN_VOID(context, "get object failed", NapiAVSessionManager::errcode_[ERR_INVALID_PARAM]);
    };
    context->GetCbInfo(env, info, inputParser);
    context->taskId = NAPI_CAST_PICKER_HELPER_TASK_ID;
    
    auto executor = [context]() {
        auto* napiAVCastPickerHelper = reinterpret_cast<NapiAVCastPickerHelper*>(context->native);
        AAFwk::Want request;
        BuildPickerRequest(context->napiAVCastPickerOptions, request);

        PickerCallBack pickerCallBack;
        auto callback = std::make_shared<ModalUICallback>(napiAVCastPickerHelper->uiContent_, pickerCallBack);
        auto extensionCallback = CreateExtensionCallback(callback, napiAVCastPickerHelper);

        Ace::ModalUIExtensionConfig config;
        config.isProhibitBack = false;
        CHECK_AND_RETURN_LOG(napiAVCastPickerHelper->uiContent_ != nullptr, "uiContent_ is destroyed");
        napiAVCastPickerHelper->sessionId_ =
            napiAVCastPickerHelper->uiContent_->CreateModalUIExtension(request, extensionCallback, config);
        callback->SetSessionId(napiAVCastPickerHelper->sessionId_);
    };
    auto complete = [env](napi_value& output) { output = NapiUtils::GetUndefinedValue(env); };
    return NapiAsyncWork::Enqueue(env, context, "SelectAVPicker", executor, complete);
}

void NapiAVCastPickerHelper::BuildPickerRequest(const NapiAVCastPickerOptions& options, AAFwk::Want& request)
{
    std::string targetType = "sys/commonUI";
    request.SetParam(ABILITY_WANT_PARAMS_UIEXTENSIONTARGETTYPE, targetType);
    request.SetParam("isAVCastPickerHelper", true);
    request.SetParam("isCustomPicker", false);
    request.SetParam("AVCastPickerOptionsType", options.sessionType);
    request.SetParam("AVCastPickerOptionsStyle", options.pickerStyle);
    request.SetParam("AVCastPickerOptionsPositionX", options.menuPosition.x);
    request.SetParam("AVCastPickerOptionsPositionY", options.menuPosition.y);
    request.SetParam("AVCastPickerOptionsPositionW", options.menuPosition.width);
    request.SetParam("AVCastPickerOptionsPositionH", options.menuPosition.height);
    request.SetElementName(ABILITY_WANT_ELEMENT_NAME, "UIExtAbility");
}

Ace::ModalUIExtensionCallbacks NapiAVCastPickerHelper::CreateExtensionCallback(
    const std::shared_ptr<ModalUICallback>& callback, NapiAVCastPickerHelper* napiAVCastPickerHelper)
{
    static const std::string callBackName = "NapiAVCastPickerHelper::SelectAVPicker";
    auto isValid = napiAVCastPickerHelper->isValid_;
    auto generationToken = napiAVCastPickerHelper->pickerGeneration_;
    const uint64_t generation = generationToken->fetch_add(1) + 1;
    SLOGI("AVPicker generation helper=%{public}p generation=%{public}llu",
        static_cast<void*>(napiAVCastPickerHelper), static_cast<unsigned long long>(generation));
    auto pickerState = std::make_shared<std::atomic<int32_t>>(-1);
    auto notifyState = [napiAVCastPickerHelper, isValid, generationToken, generation, pickerState](int32_t state) {
        CHECK_AND_RETURN_LOG(isValid && *isValid, "NapiAVCastPickerHelper is destroyed");
        CHECK_AND_RETURN_LOG(generationToken->load() == generation, "Ignoring an older picker session");
        int32_t previous = pickerState->load();
        // Each modal session can appear once and disappear once. A terminal
        // close cannot be reopened by a delayed remote-ready/provider callback.
        while (previous != state && previous != static_cast<int32_t>(STATE_DISAPPEARING)) {
            if (pickerState->compare_exchange_weak(previous, state)) {
                CHECK_AND_RETURN_LOG(isValid && *isValid, "NapiAVCastPickerHelper is destroyed");
                CHECK_AND_RETURN_LOG(generationToken->load() == generation, "Ignoring an older picker session");
                SLOGI("AVPicker state helper=%{public}p generation=%{public}llu state=%{public}d",
                    static_cast<void*>(napiAVCastPickerHelper), static_cast<unsigned long long>(generation), state);
                napiAVCastPickerHelper->HandleEvent(EVENT_PICPKER_STATE_CHANGE, callBackName, state, generation);
                return;
            }
        }
    };
    return {
        .onRelease = ([callback, notifyState](auto arg) {
            // The callback still owns and closes this actual modal session,
            // even when its generation no longer owns the helper's events.
            callback->OnRelease(arg);
            notifyState(STATE_DISAPPEARING);
        }),
        .onResult = ([callback](auto arg1, auto arg2) { callback->OnResult(arg1, arg2); }),
        .onReceive = ([callback, notifyState](auto arg) {
            callback->OnReceive(arg);
            AAFwk::Want stateWant;
            stateWant.SetParams(arg);
            const int32_t state = stateWant.GetIntParam("state", -1);
            if (state == static_cast<int32_t>(STATE_APPEARING) ||
                state == static_cast<int32_t>(STATE_DISAPPEARING)) {
                notifyState(state);
            }
        }),
        .onError = ([callback](auto arg1, auto arg2, auto arg3) { callback->OnError(arg1, arg2, arg3); }),
        .onRemoteReady = ([callback](auto arg) {
            // Proxy readiness does not prove that the provider's panel is
            // visible. Its actual sendData({state: 0}) owns APPEARING.
            callback->OnRemoteReady(arg);
        }),
        .onDestroy = ([callback, notifyState]() {
            callback->OnDestroy();
            notifyState(STATE_DISAPPEARING);
        }),
    };
}

napi_value NapiAVCastPickerHelper::ResetCommunicationDevice(napi_env env, napi_callback_info info)
{
    struct ConcreteContext : public ContextBase {
        sptr<AudioStandard::AudioRendererFilter> audioRendererFilter;
    };
    auto context = std::make_shared<ConcreteContext>();
    auto inputParser = [env, context](size_t argc, napi_value* argv) {
        CHECK_ARGS_RETURN_VOID(context, argc == ARGC_ZERO,
                               "invalid argument number", NapiAVSessionManager::errcode_[ERR_INVALID_PARAM]);
        CHECK_RETURN_VOID(NapiUtils::GetUndefinedValue(env), "CHECK_RETURN_VOID");
    };
    context->GetCbInfo(env, info, inputParser);

    auto executor = [context]() {
        auto* napiAVCastPickerHelper = reinterpret_cast<NapiAVCastPickerHelper*>(context->native);
        context->audioRendererFilter = new (std::nothrow) AudioStandard::AudioRendererFilter();
        CHECK_AND_RETURN_LOG(context->audioRendererFilter != nullptr, "init AudioRendererFilter failed");
        (context->audioRendererFilter->rendererInfo).streamUsage =
            AudioStandard::StreamUsage::STREAM_USAGE_VOICE_COMMUNICATION;
        
        napiAVCastPickerHelper->audioRoutingMngr_->RestoreOutputDevice(context->audioRendererFilter);
    };

    auto complete = [env](napi_value& output) { output = NapiUtils::GetUndefinedValue(env); };

    return NapiAsyncWork::Enqueue(env, context, "ResetCommunicationDevice", executor, complete);
}

napi_status NapiAVCastPickerHelper::OnPickerStateChange(napi_env env, NapiAVCastPickerHelper* napiAVCastPickerHelper,
    napi_value callback)
{
    SLOGI("NapiAVCastPickerHelper OnPickerStateChange");
    CHECK_AND_RETURN_RET_LOG(napiAVCastPickerHelper != nullptr, napi_generic_failure, "input param is nullptr");
    return napiAVCastPickerHelper->AddCallback(env, EVENT_PICPKER_STATE_CHANGE, callback);
}

napi_status NapiAVCastPickerHelper::OffPickerStateChange(napi_env env, NapiAVCastPickerHelper* napiAVCastPickerHelper,
    napi_value callback)
{
    SLOGI("NapiAVCastPickerHelper OffPickerStateChange");
    CHECK_AND_RETURN_RET_LOG(napiAVCastPickerHelper != nullptr, napi_generic_failure, "input param is nullptr");
    return napiAVCastPickerHelper->RemoveCallback(env, EVENT_PICPKER_STATE_CHANGE, callback);
}

template<typename T>
void NapiAVCastPickerHelper::HandleEvent(int32_t event, std::string callBackName, const T& param, uint64_t generation)
{
    std::lock_guard<std::mutex> lockGuard(lock_);
    const auto generationToken = pickerGeneration_;
    CHECK_AND_RETURN_LOG(generationToken->load() == generation, "Ignoring an older picker event");
    if (callbacks_[event].empty()) {
        SLOGE("pickerNotRegister:%{public}d", event);
        return;
    }
    for (auto ref = callbacks_[event].begin(); ref != callbacks_[event].end(); ++ref) {
        const auto registered = callbackRefs_.find(*ref);
        if (registered == callbackRefs_.end()) {
            continue;
        }
        const auto callbackRef = registered->second;
        // DataContextWithFunc retains napi_ref&; its check owns this cell.
        SLOGI("AVPicker queue helper=%{public}p generation=%{public}llu ref=%{public}p cell=%{public}p",
            static_cast<void*>(this), static_cast<unsigned long long>(generation),
            static_cast<void*>(*callbackRef), static_cast<void*>(callbackRef.get()));
        asyncCallback_->CallWithFunc(*callbackRef, isValid_,
            [this, callbackRef, event, generationToken, generation]() {
                std::lock_guard<std::mutex> lockGuard(lock_);
                if (generationToken->load() != generation) {
                    SLOGI("Ignoring an older queued picker event");
                    SLOGI("AVPicker reject helper=%{public}p generation=%{public}llu ref=%{public}p cell=%{public}p reason=generation",
                        static_cast<void*>(this), static_cast<unsigned long long>(generation),
                        static_cast<void*>(*callbackRef), static_cast<void*>(callbackRef.get()));
                    return false;
                }
                const auto registered = callbackRefs_.find(*callbackRef);
                if (registered == callbackRefs_.end() || registered->second != callbackRef) {
                    SLOGI("AVPicker reject helper=%{public}p generation=%{public}llu ref=%{public}p cell=%{public}p reason=registration",
                        static_cast<void*>(this), static_cast<unsigned long long>(generation),
                        static_cast<void*>(*callbackRef), static_cast<void*>(callbackRef.get()));
                    return false;
                }
                if (callbacks_[event].empty()) {
                    SLOGE("checkCallbackValid with empty list for event=%{public}d", event);
                    return false;
                }
                bool hasFunc = false;
                for (auto it = callbacks_[event].begin(); it != callbacks_[event].end(); ++it) {
                    hasFunc = (*callbackRef == *it ? true : hasFunc);
                }
                if (!hasFunc) {
                    SLOGE("checkCallbackValid res false for event=%{public}d", event);
                }
                SLOGI("AVPicker checked helper=%{public}p generation=%{public}llu ref=%{public}p cell=%{public}p present=%{public}d",
                    static_cast<void*>(this), static_cast<unsigned long long>(generation),
                    static_cast<void*>(*callbackRef), static_cast<void*>(callbackRef.get()), hasFunc);
                return hasFunc;
            }, callBackName,
            [param](napi_env env, int& argc, napi_value* argv) {
                argc = NapiUtils::ARGC_ONE;
                NapiUtils::SetValue(env, param, *argv);
            });
    }
}

napi_status NapiAVCastPickerHelper::AddCallback(napi_env env, int32_t event, napi_value callback)
{
    SLOGI("Add callback %{public}d", event);
    std::lock_guard<std::mutex> lockGuard(lock_);
    CHECK_AND_RETURN_RET_LOG(event == EVENT_PICPKER_STATE_CHANGE, napi_generic_failure, "has no event");
    napi_ref ref = nullptr;
    CHECK_AND_RETURN_RET_LOG(napi_ok == NapiUtils::GetRefByCallback(env, callbacks_[event], callback, ref),
                             napi_generic_failure, "get callback reference failed");
    CHECK_AND_RETURN_RET_LOG(ref == nullptr, napi_ok, "callback has been registered");
    napi_status status = napi_create_reference(env, callback, NapiUtils::ARGC_ONE, &ref);
    if (status != napi_ok) {
        SLOGE("napi_create_reference failed");
        return status;
    }
    if (asyncCallback_ == nullptr) {
        asyncCallback_ = std::make_shared<NapiAsyncCallback>(env);
        if (asyncCallback_ == nullptr) {
            SLOGE("no memory");
            return napi_generic_failure;
        }
    }
    const auto callbackRef = std::make_shared<napi_ref>(ref);
    callbackRefs_.emplace(ref, callbackRef);
    callbacks_[event].push_back(ref);
    SLOGI("AVPicker registered helper=%{public}p ref=%{public}p cell=%{public}p",
        static_cast<void*>(this), static_cast<void*>(ref), static_cast<void*>(callbackRef.get()));
    return napi_ok;
}

napi_status NapiAVCastPickerHelper::RemoveCallback(napi_env env, int32_t event, napi_value callback)
{
    SLOGI("Remove callback %{public}d", event);
    std::lock_guard<std::mutex> lockGuard(lock_);
    CHECK_AND_RETURN_RET_LOG(event == EVENT_PICPKER_STATE_CHANGE, napi_generic_failure, "has no event");
    if (callback == nullptr) {
        for (auto callbackRef = callbacks_[event].begin(); callbackRef != callbacks_[event].end();) {
            napi_status ret = napi_delete_reference(env, *callbackRef);
            const auto registered = callbackRefs_.find(*callbackRef);
            SLOGI("AVPicker removed helper=%{public}p ref=%{public}p cell=%{public}p status=%{public}d",
                static_cast<void*>(this), static_cast<void*>(*callbackRef),
                registered == callbackRefs_.end() ? nullptr : static_cast<void*>(registered->second.get()), ret);
            CHECK_AND_RETURN_RET_LOG(napi_ok == ret, ret, "delete callback reference failed");
            callbackRefs_.erase(*callbackRef);
            callbackRef = callbacks_[event].erase(callbackRef);
        }
        callbacks_[event].clear();
        return napi_ok;
    }
    napi_ref ref = nullptr;
    CHECK_AND_RETURN_RET_LOG(napi_ok == NapiUtils::GetRefByCallback(env, callbacks_[event], callback, ref),
                             napi_generic_failure, "get callback reference failed");
    CHECK_AND_RETURN_RET_LOG(ref != nullptr, napi_ok, "callback has been remove");
    callbacks_[event].remove(ref);
    const auto registered = callbackRefs_.find(ref);
    const auto removedCell = registered == callbackRefs_.end() ? nullptr : registered->second;
    callbackRefs_.erase(ref);
    const napi_status status = napi_delete_reference(env, ref);
    SLOGI("AVPicker removed helper=%{public}p ref=%{public}p cell=%{public}p status=%{public}d",
        static_cast<void*>(this), static_cast<void*>(ref), static_cast<void*>(removedCell.get()), status);
    return status;
}

bool NapiAVCastPickerHelper::IsCallbacksEmpty(int32_t event)
{
    std::lock_guard<std::mutex> lockGuard(lock_);
    CHECK_AND_RETURN_RET_LOG(event == EVENT_PICPKER_STATE_CHANGE, true, "has no event");
    return callbacks_[event].empty();
}

ModalUICallback::ModalUICallback(Ace::UIContent* uiContent, PickerCallBack& pickerCallBack)
{
    this->uiContent_ = uiContent;
    this->pickerCallBack_ = pickerCallBack;
}

void ModalUICallback::SetSessionId(int32_t sessionId)
{
    this->sessionId_ = sessionId;
}

void ModalUICallback::OnRelease(int32_t releaseCode)
{
    SLOGI("ModalUICallback OnRelease enter. release code is %{public}d", releaseCode);
    this->uiContent_->CloseModalUIExtension(this->sessionId_);
    pickerCallBack_.ready = true;
}

void ModalUICallback::OnResult(int32_t resultCode, const OHOS::AAFwk::Want& result)
{
    SLOGI("ModalUICallback OnResult enter. resultCode code is %{public}d", resultCode);
    pickerCallBack_.resultCode = resultCode;
}

void ModalUICallback::OnReceive(const OHOS::AAFwk::WantParams& request)
{
    SLOGI("ModalUICallback OnReceive enter.");
}

void ModalUICallback::OnError(int32_t code, const std::string& name, const std::string& message)
{
    SLOGI("ModalUICallback OnError enter. errorCode=%{public}d, name=%{public}s, message=%{public}s",
        code, name.c_str(), message.c_str());
    if (!pickerCallBack_.ready) {
        this->uiContent_->CloseModalUIExtension(this->sessionId_);
        pickerCallBack_.ready = true;
    }
}

void ModalUICallback::OnRemoteReady(const std::shared_ptr<Ace::ModalUIExtensionProxy>& uiProxy)
{
    SLOGI("ModalUICallback OnRemoteReady enter.");
}

void ModalUICallback::OnDestroy()
{
    SLOGI("ModalUICallback OnDestroy enter.");
}

} // namespace OHOS::AVSession
