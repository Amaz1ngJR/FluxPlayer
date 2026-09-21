-- Single-file FluxPlayer skin.
local skin = {
    schemaVersion = 1,
    id = "minimal-lua",
    name = "Minimal Lua",
    version = "1.0.0",
    author = "FluxPlayer",
    description = "Lua-driven minimal HomeScreen example.",
    compatibility = {
        skinApi = 1,
        surfaces = { "home", "opening", "player", "merge", "settings", "subtitle", "toast" },
    },
    roles = {
        background = {
            void = "#071018",
            canvas = "#0A1722",
            panel = "#0D2230",
            panelRaised = "#123142",
            panelTransparent = "rgba(13,34,48,0.92)",
            radialCenter = "#071018",
            radialMiddle = "#0A1722",
            radialOuter = "#071018",
            localButton = "#0D2230",
            mergeButton = "#123142",
            field = "#071018",
        },
        accent = {
            primary = "#31D7C8",
            primarySoft = "#68EADD",
            primaryDim = "#176B66",
            secondary = "#FFB84D",
            tertiary = "#FF6B6B",
        },
        text = {
            primary = "#F2FBFA",
            secondary = "#B8D7D4",
            muted = "#769996",
            disabled = "#405B59",
            topStatus = "#769996",
            panelDescription = "#B8D7D4",
            section = "#769996",
            fieldHint = "#769996",
            historyTitle = "#F2FBFA",
            historyDisabled = "#B8D7D4",
            footer = "#769996",
            separator = "#405B59",
            violetDim = "rgba(255,184,77,0.68)",
        },
        state = {
            recording = "#FF6B6B",
            warning = "#FFB84D",
            error = "#FF6B6B",
            success = "#31D7C8",
        },
        line = {
            subtle = "rgba(118,153,150,0.25)",
            primary = "rgba(49,215,200,0.72)",
            secondary = "rgba(255,184,77,0.68)",
            cyanDim = "rgba(49,215,200,0.70)",
            cyanHalf = "rgba(49,215,200,0.50)",
        },
    },
    gradients = {
        primaryRail = { "#31D7C8", "#68EADD" },
        dockEdge = { "rgba(49,215,200,0)", "#31D7C8", "#FFB84D", "rgba(255,184,77,0)" },
        panelHeader = { "rgba(49,215,200,0)", "#31D7C8", "#FFB84D", "rgba(255,184,77,0)" },
        homeRail = { "rgba(49,215,200,0)", "#31D7C8", "#FFB84D", "rgba(255,184,77,0)" },
        homeCyanBorder = { "#31D7C8", "rgba(49,215,200,0.50)", "rgba(49,215,200,0.12)" },
        homeVioletBorder = { "rgba(255,184,77,0.16)", "rgba(255,184,77,0.65)", "#FF6B6B" },
        homeEnergy = { "rgba(49,215,200,0)", "#31D7C8", "#F2FBFA", "#FFB84D", "rgba(255,184,77,0)" },
        homeBridge = { "rgba(49,215,200,0.50)", "rgba(255,184,77,0.50)" },
    },
    -- metrics 是 C++ 唯一读取的几何来源（用于 ApplyImGuiStyle 设置 ImGui 全局样式）。
    -- 界面尺寸不放这里：那是皮肤私有的 surfaces.<name> 数据。
    metrics = {
        radius = { panel = 8, popup = 5, button = 5 },
        spacing = { panelPadding = 18, controlGap = 8, rowGap = 5 },
        opacity = { popup = 0.97 },
    },
    surfaces = {
        home = {
            cardPaddingX = 32,
            cardPaddingY = 28,
            panelTopRailHeight = 4,
            panelBottomRailHeight = 3,
            innerBorderInset = 6,
            cornerLength = 24,
            cornerThickness = 2.5,
            titleToActionGap = 18,
            localButtonW = 320,
            localButtonH = 48,
            sectionGap = 12,
            separatorWidthRatio = 0.6,
            separatorOffsetY = 4,
            separatorAfterGap = 5,
            urlLabelGap = 4,
            urlButtonW = 90,
            urlRowGap = 8,
            urlFramePaddingX = 14,
            urlFramePaddingY = 10,
            errorGap = 8,
            footerBottomGap = 4,
            loginModalW = 440,
            loginButtonH = 30,
            loginStoredButtonW = 140,
            loginRetryButtonW = 110,
            loginOpenButtonW = 120,
            loginChoiceButtonW = 150,
            gridHorizonRatio = 0.42,
            gridRows = 16,
            gridColumns = 18,
            scanlineStep = 3,
            screenTopRailHeight = 3,
            screenBottomRailHeight = 2,
            particleCount = 60,
            particleRadius = 1.2,
            titleAreaH = 176,
            panelGapRatio = 0.039,
            sidePadRatio = 0.067,
            bottomPad = 106,
            leftPanelRatio = 0.55,
            minPanelAreaW = 400,
            minPanelH = 200,
            emblemY = 86,
            titleInset = 68,
            subtitleY = 122,
            leftPanelCut = 32,
            rightPanelCut = 28,
            backgroundRailH = 2.5,
            cornerLong = 56,
            cornerShort = 38,
            cornerInset = 28,
            statusTopInsetX = 48,
            statusTopY = 16,
            statusBottomInsetX = 102,
            statusBottomY = 18,
        },
        opening = {
            maxWidthRatio = 0.7,
            overlayAlpha = 0.78,
            titlePx = 28,
            titleOffsetY = 22,
            phaseOffsetY = 78,
            sourceOffsetY = 108,
            dotsBottomOffset = 36,
            dotsGap = 14,
            dotRadius = 4,
            cornerLength = 18,
            cornerThickness = 1.5,
            redrawIntervalMs = 100,
        },
        player = {
            dockPaddingX = 8,
            dockPaddingY = 4,
            dockRailHeight = 2,
            dockRowGap = 4,
            progressHeadRadius = 5,
            progressGlowRadius = 8,
            progressOuterGlowRadius = 12,
            progressTooltipGap = 5,
            stopButtonW = 60,
            recordIdleButtonW = 60,
            recordActiveButtonW = 70,
            toolButtonW = 60,
            volumeSliderW = 150,
            volumeButtonExtraW = 4,
            toolbarGap = 4,
            toolbarRightMargin = 12,
            downloadButtonW = 72,
            downloadBarW = 120,
            downloadBarGap = 8,
            downloadInfoGap = 6,
            brightnessSliderW = 50,
            brightnessSliderH = 162,
            brightnessPopupGap = 8,
        },
        settings = {
            maxWidth = 920,
            maxHeight = 640,
            widthRatio = 0.92,
            heightRatio = 0.88,
            overlayAlpha = 0.59,
            panelAlpha = 0.97,
            paddingX = 24,
            paddingY = 18,
            itemGapX = 10,
            itemGapY = 8,
            sectionGap = 6,
            sectionLabelGap = 2,
            footerReserve = 80,
            titleRailGap = 4,
            titleRailHeight = 2,
            navWidth = 136,
            navGap = 16,
            navButtonH = 34,
            navButtonGap = 6,
            closeButtonW = 28,
            closePaddingX = 4,
            closePaddingY = 2,
            comboPaddingX = 10,
            comboPaddingY = 8,
            actionPaddingX = 12,
            actionPaddingY = 10,
            mediumFieldRatio = 0.55,
            wideFieldRatio = 0.75,
            pathFieldRatio = 0.62,
            compactFieldRatio = 0.3,
            logLevelFieldRatio = 0.4,
            activeCardH = 64,
            activeCardPadding = 12,
            activeCardTextGap = 2,
            activeCardAfterGap = 10,
            statusBarH = 32,
            statusDotInsetX = 12,
            statusDotRadius = 4,
            statusTextInsetX = 24,
        },
        subtitle = {
            bottomMarginWithUi = 104,
            bottomMarginNoUi = 24,
            widthRatio = 0.85,
            backgroundAlpha = 0.55,
        },
    },
    -- motion：C++ 只消费这两项（dock 自动隐藏、热加载防抖）；动画时序由皮肤自算。
    -- motion：前两项由 C++ 消费，pulseSpeed 供皮肤自己使用。
    motion = {
        autoHideDelaySeconds = 3,
        reloadDebounceMs = 160,
        pulseSpeed = 2.6,
    },
    -- typography：字体文件走 assets.*，这里只列字号（titlePx/bodyPx 经 ui.getSkin 暴露）。
    typography = {
        titlePx = 38,
        bodyPx = 13,
        buttonPx = 12,
    },
}

local function render_home()
    local width, height = ui.getWindowSize()
    local url = state.get("url") or ""

    return ui.container {
        layout = "vbox",
        align = "center",
        gap = 14,
        padding = { top = math.max(48, height * 0.14), left = 40, right = 40, bottom = 40 },
        color = "bgVoid",

        ui.text {
            content = "FLUX PLAYER",
            font = "display",
            size = 38,
            color = "textPrimary",
            align = "center",
            height = 56
        },
        ui.text {
            content = "LUA-DRIVEN MEDIA CONSOLE",
            color = "textMuted",
            align = "center",
            height = 26
        },
        ui.spacer { height = 24 },
        ui.button {
            text = "OPEN LOCAL FILE",
            action = "openLocalFile",
            theme = "primary",
            width = math.min(360, width - 80),
            height = 48,
            align = "center"
        },
        ui.button {
            text = "MERGE VIDEOS",
            action = "openMerge",
            theme = "secondary",
            width = math.min(360, width - 80),
            height = 44,
            align = "center"
        },
        ui.input {
            id = "url",
            stateKey = "url",
            value = url,
            placeholder = "https://... or rtsp://...",
            width = math.min(360, width - 80),
            height = 38,
            align = "center"
        },
        ui.button {
            text = "PLAY URL",
            action = "playUrl",
            payload = { url = url },
            theme = "primary",
            width = math.min(180, width - 80),
            height = 40,
            align = "center"
        },
        ui.button {
            text = "SETTINGS",
            action = "openSettings",
            theme = "muted",
            width = 140,
            height = 36,
            align = "center"
        },
        ui.spacer { height = 0 },
        ui.text {
            content = "v" .. app.version,
            color = "textMuted",
            align = "center",
            height = 24
        }
    }
end

-- 设置项候选项由 C++ 以 "value\tlabel\tvalue\tlabel…" 一行下发
local function optionsFromRow(row)
    local tokens = {}
    for piece in string.gmatch(row.options or "", "[^\t]+") do tokens[#tokens + 1] = piece end
    local opts = {}
    for i = 1, #tokens, 2 do
        local value, label = tokens[i], tokens[i + 1] or tokens[i]
        if value and value ~= "" then opts[#opts + 1] = { value = value, label = label } end
    end
    return opts
end

-- 开场 splash：与 cyberpunk-neon 同样的数据契约（mediaPath / phase / elapsed）。
local function render_opening()
    local width, height = ui.getWindowSize()
    local data = (ui.getData("opening") or {})[1] or {}
    local t = tonumber(data.elapsed) or 0
    local cardW, cardH = math.min(520, width * 0.7), 170
    local cx, cy = (width - cardW) / 2, (height - cardH) / 2
    local children = {
        ui.rect { color = "bgVoid", opacity = 0.78, x = 0, y = 0, width = width, height = height, absolute = true },
        ui.rect { color = "bgPanel", opacity = 0.96, rounding = 6,
            x = cx, y = cy, width = cardW, height = cardH, absolute = true },
        ui.rect { color = "accentPrimary", outline = true, thickness = 1, rounding = 6,
            x = cx, y = cy, width = cardW, height = cardH, absolute = true },
        ui.text { content = "OPENING", color = "accentPrimary", size = 28,
            x = cx, y = cy + 28, width = cardW, height = 34, align = "center", absolute = true },
        ui.text { content = data.phase or "", color = "textPrimary", size = 12, baseline = 12,
            x = cx, y = cy + 74, width = cardW, height = 18, align = "center", absolute = true },
        ui.text { content = data.mediaPath or "", color = "textMuted", size = 11, baseline = 11,
            x = cx, y = cy + 100, width = cardW, height = 16, align = "center", ellipsis = true, absolute = true },
    }
    for i = 0, 2 do
        local alpha = 0.35 + 0.55 * (0.5 + 0.5 * math.sin(t * 2 - i * 0.4))
        children[#children + 1] = ui.rect { color = "accentPrimary", opacity = alpha, rounding = 4,
            x = cx + cardW * 0.5 + (i - 1) * 14 - 4, y = cy + cardH - 32, width = 8, height = 8, absolute = true }
    end
    return ui.container { layout = "absolute", width = width, height = height, table.unpack(children) }
end

skin.surfaces.home.render = render_home

local function render_player()
    local width, height = ui.getWindowSize()
    local player = (ui.getData("player") or {})[1] or {}
    local progress = math.max(0, math.min(1, tonumber(player.progress) or 0))
    local dockY = math.max(0, height - 88)
    return ui.container { layout = "absolute", width = width, height = height,
        ui.rect { color = "bgPanel", x = 0, y = dockY, width = width, height = 88, absolute = true },
        ui.slider { id = "playerSeek", value = progress, min = 0, max = 1,
            x = 16, y = dockY + 10, width = width - 32, height = 18, absolute = true },
        ui.button { text = player.state == "playing" and "PAUSE" or "PLAY", action = "togglePlayback",
            theme = "primary", x = 20, y = dockY + 38, width = 100, height = 34, absolute = true },
        ui.button { text = "STOP", action = "stop", theme = "secondary",
            x = 130, y = dockY + 38, width = 90, height = 34, absolute = true },
        ui.button { text = "SETTINGS", action = "openSettings", theme = "secondary",
            x = width - 130, y = dockY + 38, width = 110, height = 34, absolute = true }
    }
end

local function render_merge()
    local width, height = ui.getWindowSize()
    local status = (ui.getData("merge") or {})[1] or {}
    local clips = ui.getData("mergeClips") or {}
    local children = {
        ui.rect { color = "bgCanvas", x = 0, y = 0, width = width, height = height, absolute = true },
        ui.text { content = "MERGE VIDEOS", font = "display", color = "textPrimary",
            x = 40, y = 30, width = width - 80, height = 50, size = 28, absolute = true },
        ui.button { text = "+ ADD FILES", action = "mergeAddFiles", theme = "primary",
            x = 40, y = height - 64, width = 160, height = 38, absolute = true },
        ui.button { text = "START", action = "mergeStart", theme = "secondary",
            x = width - 330, y = height - 64, width = 140, height = 38, absolute = true },
        ui.button { text = "BACK", action = "mergeBack", theme = "secondary",
            x = width - 180, y = height - 64, width = 140, height = 38, absolute = true },
    }
    local y = 100
    for i, clip in ipairs(clips) do
        if i > 10 then break end
        children[#children + 1] = ui.button { text = tostring(i) .. ". " .. (clip.name or "clip"),
            action = "mergeSelect", payload = { index = tostring(i) }, theme = "primary", textAlign = "left",
            x = 60, y = y, width = width - 170, height = 42, absolute = true }
        children[#children + 1] = ui.button { text = "X", action = "mergeRemove", payload = { index = tostring(i) },
            theme = "danger", x = width - 100, y = y, width = 40, height = 42, absolute = true }
        y = y + 48
    end
    if status.phase == "merging" then
        children[#children + 1] = ui.text { content = "MERGING " .. tostring(math.floor((tonumber(status.progress) or 0) * 100)) .. "%",
            color = "textPrimary", x = 40, y = height - 110, width = width - 80, height = 30, absolute = true }
    end
    return ui.container { layout = "absolute", width = width, height = height, table.unpack(children) }
end

-- Toast 浮层：右上角堆叠。尺寸/落位/滑入偏移都按 C++ 下发的数值走，
-- 皮肤只挑配色，这样换肤不会让动画和 C++ 回退版本表现不一致。
local function render_toast()
    local width, height = ui.getWindowSize()
    local rows = ui.getData("toasts") or {}
    local toastW, toastH = 400, 90
    local accent = { info = "accentPrimary", success = "stateSuccess",
                     warning = "stateWarning", error = "stateError" }
    local children = {}
    local y = 20
    for _, toast in ipairs(rows) do
        local alpha = math.max(0, math.min(1, tonumber(toast.alpha) or 0))
        local x = width - toastW - 20 + (tonumber(toast.slide) or 0)
        local role = accent[toast.type] or "accentPrimary"
        children[#children + 1] = ui.rect { color = "bgPanel", opacity = 0.95 * alpha, rounding = 8,
            x = x, y = y, width = toastW, height = toastH, absolute = true }
        children[#children + 1] = ui.rect { color = role, outline = true, thickness = 1, rounding = 8,
            opacity = alpha * 0.55, x = x, y = y, width = toastW, height = toastH, absolute = true }
        children[#children + 1] = ui.text { content = toast.title or "", color = "textPrimary", size = 13,
            baseline = 13, x = x + 15, y = y + 12, width = toastW - 30, height = 18, absolute = true }
        if (toast.content or "") ~= "" then
            children[#children + 1] = ui.text { content = toast.content, color = "textPrimary", size = 12,
                baseline = 12, x = x + 15, y = y + 34, width = toastW - 30, height = 17, absolute = true }
        end
        if (toast.detail or "") ~= "" then
            children[#children + 1] = ui.text { content = toast.detail, color = "textMuted", size = 11,
                baseline = 11, x = x + 15, y = y + 54, width = toastW - 30, height = 16, absolute = true }
        end
        y = y + toastH + 10
    end
    return ui.container { layout = "absolute", width = width, height = height, table.unpack(children) }
end

-- 设置面板：与 cyberpunk-neon 同构（都用 ui.getData("settings") 驱动），
-- 只在外观上做简化——左侧导航换成一行横向翻页按钮，配色沿用本皮肤的角色名。
local function render_settings()
    local width, height = ui.getWindowSize()
    local rows = ui.getData("settings") or {}
    local input = ui.getInput()
    local n = function(v) return tonumber(v) or 0 end

    local panelW = math.min(920, width * 0.92)
    local panelH = math.min(640, height * 0.88)
    local px, py = (width - panelW) / 2, (height - panelH) / 2

    local groupNames = { "playback", "subtitle", "capture", "proxy", "logging", "appearance" }
    local current = state.get("settingsGroup") or "playback"
    local known = false
    for _, name in ipairs(groupNames) do if name == current then known = true end end
    if not known then current = "playback" end

    local children = {
        ui.rect { color = "bgVoid", opacity = 0.59, x = 0, y = 0, width = width, height = height, absolute = true },
        ui.rect { color = "bgPanel", opacity = 0.97, rounding = 6,
            x = px, y = py, width = panelW, height = panelH, absolute = true },
        ui.rect { color = "accentPrimary", outline = true, thickness = 1, rounding = 6,
            x = px, y = py, width = panelW, height = panelH, absolute = true },
        ui.text { content = "SETTINGS", color = "accentPrimary", size = 15, baseline = 15,
            x = px + 24, y = py + 18, width = 300, height = 22, absolute = true },
        ui.button { text = "X", action = "closeSettings", theme = "secondary",
            x = px + panelW - 52, y = py + 16, width = 28, height = 22, absolute = true },
    }

    -- 横向分组按钮：窄窗口下比左侧导航更省空间
    local tabY = py + 56
    local tabW = (panelW - 48) / #groupNames
    for i, name in ipairs(groupNames) do
        local tx = px + 24 + (i - 1) * tabW
        children[#children + 1] = ui.button {
            text = string.upper(name), action = "setUiState",
            payload = { key = "settingsGroup", value = name },
            theme = name == current and "primary" or "secondary",
            x = tx, y = tabY, width = tabW - 4, height = 28, absolute = true }
    end

    local listY = tabY + 44
    local listH = panelH - (listY - py) - 60
    local scrollKey = "settingsScroll_" .. current

    local visible = {}
    for _, row in ipairs(rows) do
        if row.group == current then visible[#visible + 1] = row end
    end

    -- section 标题也要占高度（对应旧版页内的小节分隔）
    local rowH, rowSection, contentH = {}, {}, 0
    local lastSection = nil
    for i, row in ipairs(visible) do
        local sec = row.section or ""
        if sec ~= "" and sec ~= lastSection then
            lastSection = sec
            rowSection[i] = true
            contentH = contentH + 30
        end
        local h = (row.type == "bool") and 34 or 52
        if (row.hint or "") ~= "" then h = h + 18 end
        rowH[i] = h
        contentH = contentH + h + 8
    end
    local maxScroll = math.max(0, contentH - listH)
    local offset = math.min(n(state.get(scrollKey)), maxScroll)
    if input.wheel and input.wheel ~= 0 then
        local inside = input.x >= px + 24 and input.x <= px + panelW - 24 and
                       input.y >= listY and input.y <= listY + listH
        if inside then
            offset = math.max(0, math.min(maxScroll, offset - input.wheel * 30))
            state.set(scrollKey, tostring(offset))
        end
    end

    local y = listY - offset
    for i, row in ipairs(visible) do
        if rowSection[i] then
            if y + 30 >= listY and y <= listY + listH then
                children[#children + 1] = ui.text { content = row.section, color = "accentPrimary",
                    size = 11, baseline = 11, x = px + 24, y = y + 4,
                    width = panelW - 48, height = 16, absolute = true }
            end
            y = y + 30
        end
        local h = rowH[i]
        if y + h >= listY and y <= listY + listH then
            local controlW = math.min(panelW * 0.52, 320)
            if row.type == "bool" then
                -- 复选框在左、标签在右，两者垂直居中（避免文字贴着方框）
                children[#children + 1] = ui.checkbox { id = row.key, action = "configChanged",
                    text = "", value = (row.value == "true"),
                    x = px + 24, y = y + 7, width = 24, height = 22, absolute = true }
                children[#children + 1] = ui.text { content = row.label or row.key, color = "textPrimary",
                    size = 12, baseline = 12, x = px + 56, y = y + 10,
                    width = panelW - 88, height = 18, absolute = true }
                if (row.hint or "") ~= "" then
                    children[#children + 1] = ui.text { content = row.hint, color = "textMuted",
                        size = 11, baseline = 11, x = px + 56, y = y + 30,
                        width = panelW - 88, height = 16, absolute = true }
                end
            else
                -- 标签独占一行，控件下一行
                children[#children + 1] = ui.text { content = row.label or row.key, color = "textPrimary",
                    size = 12, baseline = 12, x = px + 24, y = y + 4,
                    width = panelW - 48, height = 18, absolute = true }
                local ctrlY = y + 22
                if row.type == "option" then
                    children[#children + 1] = ui.combo { id = row.key, action = "configChanged",
                        value = row.value, options = optionsFromRow(row),
                        x = px + 24, y = ctrlY, width = controlW, height = 30, absolute = true }
                elseif row.type == "int" then
                    -- 纯文本框，无 -/+ 步进按钮（后端按失焦/回车提交）。step 已不再被后端读取。
                    children[#children + 1] = ui.numberInput { id = row.key, action = "configChanged",
                        value = n(row.value), min = n(row.min), max = n(row.max),
                        x = px + 24, y = ctrlY, width = math.min(controlW, 180), height = 30, absolute = true }
                elseif row.type == "number" then
                    children[#children + 1] = ui.slider { id = row.key, action = "configChanged",
                        value = n(row.value), min = n(row.min), max = n(row.max),
                        x = px + 24, y = ctrlY + 5, width = math.min(controlW, 240), height = 20, absolute = true }
                else
                    -- 路径类设置：右侧跟一个 Browse 按钮，弹哪种选择器由 C++ 下发的
                    -- row.pathKind 决定（none/file/dir），皮肤不硬编码 key。
                    local kind = row.pathKind or "none"
                    local hasBrowse = (kind == "file" or kind == "dir")
                    local fieldW = math.min(controlW, 420)
                    local btnW, btnGap = 72, 8
                    if hasBrowse then
                        fieldW = math.max(120, fieldW - btnW - btnGap)
                    end
                    children[#children + 1] = ui.input { id = row.key, stateKey = row.key,
                        value = row.value, action = "configChanged",
                        x = px + 24, y = ctrlY, width = fieldW, height = 30, absolute = true }
                    if hasBrowse then
                        children[#children + 1] = ui.button { text = "Browse", action = "browsePath",
                            payload = { key = row.key }, theme = "secondary",
                            x = px + 24 + fieldW + btnGap, y = ctrlY + 2, width = btnW, height = 26,
                            absolute = true }
                    end
                end
                if (row.hint or "") ~= "" then
                    children[#children + 1] = ui.text { content = row.hint, color = "textMuted",
                        size = 11, baseline = 11, x = px + 24, y = ctrlY + 34,
                        width = panelW - 48, height = 16, absolute = true }
                end
            end
        end
        y = y + h + 8
    end

    return ui.container { layout = "absolute", width = width, height = height, table.unpack(children) }
end

skin.surfaces.player = skin.surfaces.player or {}
skin.surfaces.player.render = render_player
skin.surfaces.merge = skin.surfaces.merge or {}
skin.surfaces.merge.render = render_merge
skin.surfaces.toast = skin.surfaces.toast or {}
skin.surfaces.toast.render = render_toast
skin.surfaces.opening = skin.surfaces.opening or {}
skin.surfaces.opening.render = render_opening
skin.surfaces.settings = skin.surfaces.settings or {}
skin.surfaces.settings.render = render_settings


return skin
