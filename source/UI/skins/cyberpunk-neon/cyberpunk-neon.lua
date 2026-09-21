-- FluxPlayer single-file skin.
-- Edit this file while the player is running; SkinManager atomically reloads valid changes.
local skin = {
    schemaVersion = 1,
    id = "cyberpunk-neon",
    name = "Cyberpunk Neon",
    version = "1.0.0",
    author = "FluxPlayer",
    description = "Default neon drive console with cyan, violet and magenta HUD accents.",
    compatibility = {
        skinApi = 1,
        surfaces = { "home", "opening", "player", "merge", "settings", "subtitle", "toast" },
    },
    roles = {
        background = {
            void = "#030511",
            canvas = "#050816",
            panel = "rgba(5, 10, 26, 0.96)",
            panelRaised = "#0A1230",
            panelTransparent = "rgba(7, 13, 34, 0.88)",
            radialCenter = "#071B31",
            radialMiddle = "#050A18",
            radialOuter = "#02040E",
            localButton = "#061329",
            mergeButton = "#0C0820",
            field = "#030817",
        },
        accent = {
            primary = "#00E8FF",
            primarySoft = "#2DA7FF",
            primaryDim = "#136C8F",
            secondary = "#A855FF",
            tertiary = "#FF3DF2",
        },
        text = {
            primary = "#EAF8FF",
            secondary = "#D0E8F8",
            muted = "#5A7A94",
            disabled = "#3A4E62",
            topStatus = "#3C5870",
            panelDescription = "#91ADCC",
            section = "#53708E",
            fieldHint = "#5A7A94",
            historyTitle = "#D0E8F8",
            historyDisabled = "#8AACC0",
            footer = "#3A5A74",
            separator = "#2A3E54",
            violetDim = "rgba(99, 54, 164, 0.68)",
        },
        state = {
            recording = "#FF3B7A",
            warning = "#FFB84D",
            error = "#FF3B7A",
            success = "#00E8FF",
        },
        line = {
            subtle = "rgba(90, 140, 190, 0.22)",
            primary = "rgba(0, 232, 255, 0.68)",
            secondary = "rgba(168, 85, 255, 0.62)",
            cyanDim = "rgba(19, 108, 143, 0.70)",
            cyanHalf = "rgba(19, 108, 143, 0.50)",
        },
    },
    gradients = {
        primaryRail = { "#00E8FF", "#2DA7FF", "#A855FF", "#FF3DF2" },
        dockEdge = { "rgba(0, 232, 255, 0)", "#00E8FF", "#A855FF", "rgba(255, 61, 242, 0)" },
        panelHeader = { "rgba(0, 232, 255, 0)", "rgba(0, 232, 255, 0.80)", "rgba(168, 85, 255, 0.80)", "rgba(255, 61, 242, 0)" },
        homeRail = { "rgba(0, 232, 255, 0)", "#00E8FF", "#2DA7FF", "#A855FF", "rgba(255, 61, 242, 0)" },
        homeCyanBorder = { "rgba(0, 232, 255, 0.95)", "rgba(0, 170, 218, 0.50)", "rgba(0, 232, 255, 0.12)" },
        homeVioletBorder = { "rgba(168, 85, 255, 0.16)", "rgba(168, 85, 255, 0.65)", "#FF3DF2" },
        homeEnergy = { "rgba(0, 232, 255, 0)", "#00E8FF", "#EAF8FF", "#A855FF", "rgba(168, 85, 255, 0)" },
        homeBridge = { "rgba(0, 232, 255, 0.50)", "rgba(168, 85, 255, 0.50)" },
        homeRailPositions = { 0, 0.18, 0.52, 0.78, 1 },
        homeCyanBorderPositions = { 0, 0.60, 1 },
        homeVioletBorderPositions = { 0, 0.40, 1 },
        homeEnergyPositions = { 0, 0.20, 0.50, 0.80, 1 },
    },
    -- metrics 是 C++ 唯一读取的几何来源（用于 ApplyImGuiStyle 设置 ImGui 全局样式）。
    -- 界面尺寸不放这里：那是皮肤私有的 surfaces.<name> 数据。
    metrics = {
        radius = { panel = 6, popup = 4, button = 3 },
        spacing = { panelPadding = 18, controlGap = 8, rowGap = 4 },
        opacity = { popup = 0.96 },
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
            navHintGap = 40,       -- 最后一个导航按钮到说明块的间距（版式图 390→424）
            navHintBlockH = 68,    -- 每页说明块高度（页名 + 两行正文）
            scrollBarW = 14,       -- 内容区右侧为滚动条预留的宽度
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
        -- Toast 尺寸：与 C++ ToastManager 的 kToast* 常量保持一致，改这里等于改两处
        toast = {
            width = 400,
            height = 90,
            marginRight = 20,
            marginTop = 20,
            spacing = 10,
            panelAlpha = 0.95,
            borderAlpha = 0.55,
            accentBarWidth = 3,
        },
    },
    -- motion：autoHideDelaySeconds / reloadDebounceMs 由 C++ 消费；
    -- pulseSpeed 是皮肤自己读的（home surface 的呼吸动画），不能省。
    motion = {
        autoHideDelaySeconds = 3.0,
        reloadDebounceMs = 160,
        pulseSpeed = 2.6,
    },
    -- typography：字体文件走 assets.*，这里只列字号（titlePx/bodyPx 经 ui.getSkin 暴露）。
    typography = {
        titlePx = 42,
        bodyPx = 15,
        buttonPx = 14,
    },
    assets = {
        preview = "mockup_player.svg",
    },
}

-- C++ supplies only ui/state/data/action services. This surface owns the complete home composition.
local designW, designH = 1440, 900
local function viewport(w, h)
    local scale = math.min(w / designW, h / designH)
    local ox = (w - designW * scale) * 0.5
    local oy = (h - designH * scale) * 0.5
    local function X(value) return ox + value * scale end
    local function Y(value) return oy + value * scale end
    local function S(value) return value * scale end
    return scale, X, Y, S
end

local function text(value, color, size, x, y, width, height, align, ellipsis)
    -- Keep small-window labels readable while preserving the design scale for large viewports.
    local readableSize = size >= 24 and math.max(size, 30) or math.max(size, 13)
    return ui.text { content = value, color = color, size = readableSize, x = x, y = y,
                     width = width, height = height, absolute = true,
                     textAlign = align or "left", ellipsis = ellipsis or false }
end

local function center_text(value, color, size, x, y, width, height)
    return text(value, color, size, x, y, width, height, "center")
end

local function rect(color, x, y, width, height, radius, opacity)
    return ui.rect { color = color, x = x, y = y, width = width, height = height,
                     rounding = radius or 3, opacity = opacity or 1, absolute = true }
end

local function line(color, x1, y1, x2, y2, thickness)
    return ui.line { color = color, x = x1, y = y1, x2 = x2 - x1, y2 = y2 - y1,
                     width = math.abs(x2 - x1), height = math.abs(y2 - y1),
                     absolute = true, thickness = thickness or 1 }
end

local function render_home()
    local width, height = ui.getWindowSize()
    local _, X, Y, S = viewport(width, height)
    local function F(value)
        return math.max(S(value), value <= 10 and 12 or 13)
    end
    local history = ui.getData("history") or {}
    local hardwareRows = ui.getData("hardware") or {}
    local hardware = hardwareRows[1] or {}
    local children = {}
    local time = ui.getTime()
    local pulse = 0.72 + 0.28 * (0.5 + 0.5 * math.sin(time * skin.motion.pulseSpeed))

    -- Background fills the viewport; all decorative/content geometry uses the 1440x900 design grid.
    children[#children + 1] = ui.radialGradient {
        cx = 0.5, cy = 0.42, radiusRatio = 0.7,
        colorCenter = "bgRadialCenter",
        colorMiddle = "bgRadialMiddle",
        colorOuter = "bgRadialOuter",
        middleStop = 0.55,
        x = 0, y = 0, width = width, height = height,
        absolute = true
    }

    -- Peripheral frame and the missing diagonal corner joins.
    local stroke = math.max(1, S(1.5))
    local function add_line(color, x1, y1, x2, y2, thickness)
        children[#children + 1] = line(color, X(x1), Y(y1), X(x2), Y(y2), math.max(1, S(thickness or 1)))
    end
    local function add_gradient_line(gradient, x1, y1, x2, y2, thickness, glow, phase, gradientStart, gradientEnd)
        children[#children + 1] = ui.gradientLine {
            gradient = gradient, x = X(x1), y = Y(y1), x2 = X(x2) - X(x1), y2 = Y(y2) - Y(y1),
            width = math.abs(X(x2) - X(x1)), height = math.max(1, math.abs(Y(y2) - Y(y1))),
            thickness = math.max(1, S(thickness or 1)), glow = glow or 0, phase = phase or 0,
            gradientStart = gradientStart or 0, gradientEnd = gradientEnd or 1, absolute = true
        }
    end
    add_gradient_line("homeRail", 0, 2, 1440, 2, 2.5, 0.75 * pulse, 0)
    add_gradient_line("homeRail", 0, 898, 1440, 898, 2.5, 0.75 * pulse, 0)
    add_line("linePrimary", 28, 28, 84, 28, 1.5); add_line("linePrimary", 84, 28, 100, 44, 1.5)
    add_line("linePrimary", 1412, 28, 1356, 28, 1.5); add_line("linePrimary", 1356, 28, 1340, 44, 1.5)
    add_line("linePrimary", 28, 872, 84, 872, 1.5); add_line("linePrimary", 84, 872, 100, 856, 1.5)
    add_line("linePrimary", 1412, 872, 1356, 872, 1.5); add_line("linePrimary", 1356, 872, 1340, 856, 1.5)
    add_line("accentPrimary", 28, 58, 66, 58, 1.5); add_line("accentPrimary", 1374, 58, 1412, 58, 1.5)
    add_line("accentPrimary", 28, 842, 66, 842, 1.5); add_line("accentPrimary", 1374, 842, 1412, 842, 1.5)
    children[#children + 1] = text("FLUX / LAUNCH CONSOLE", "textTopStatus", F(9), X(48), Y(10), S(310), S(20), "left")
    children[#children + 1] = text("SOURCE LINK // READY", "textTopStatus", F(9), X(1110), Y(10), S(282), S(20), "right")

    -- Header emblem: center (720,86), using logo image
    -- Outer scanning ring
    children[#children + 1] = ui.circleDashed { centerX = X(720), centerY = Y(86), radius = S(38),
        dashOn = S(2), dashOff = S(8), thickness = math.max(1, S(1.2)), phase = time * 0.28,
        glow = 0.45 * pulse, color = "linePrimary", opacity = 0.50, absolute = true }

    -- Logo image centered at (720, 86), size 72x72
    children[#children + 1] = ui.image {
        path = ui.getAssetPath("logo.png") or "",
        x = X(684), y = Y(50), width = S(72), height = S(72),
        opacity = 0.9,
        absolute = true
    }

    children[#children + 1] = text("FLUX", "textPrimary", S(38), X(504), Y(58), S(150), S(44), "right")
    children[#children + 1] = text("PLAYER", "textPrimary", S(38), X(786), Y(58), S(190), S(44), "left")
    children[#children + 1] = center_text("// By Amaz1ng v" .. app.version .. " //", "textPanelDescription", S(10), X(570), Y(113), S(300), S(20))
    add_gradient_line("homeEnergy", 484, 140, 608, 140, 1.5, 0.85 * pulse, 0, 0.00, 0.263)
    add_gradient_line("homeEnergy", 832, 140, 956, 140, 1.5, 0.85 * pulse, 0, 0.737, 1.00)
    children[#children + 1] = rect("accentPrimary", X(717), Y(137), S(6), S(6), S(3), 1)

    -- Exact panel boxes from mockup_home.svg, with horizontal border gradients and soft glow.
    children[#children + 1] = ui.gradientPanel { gradient = "homeCyanBorder", color = "bgPanel", x = X(96), y = Y(186),
        width = S(700), height = S(612), cutLeft = S(32), cutRight = S(34), cutY = S(34), absolute = true,
        thickness = math.max(1.5, S(2)), glow = 0.62 * pulse }
    children[#children + 1] = ui.panel { border = "lineCyanDim", x = X(110), y = Y(200),
        width = S(672), height = S(584), cutLeft = S(32), cutRight = S(34), cutY = S(34),
        fill = false, absolute = true, thickness = math.max(1, S(1)) }
    -- Left panel inner accent highlights
    add_line("accentPrimary", 142, 200, 290, 200, 2)
    add_line("accentPrimary", 110, 234, 110, 320, 2)
    add_line("accentPrimary", 750, 784, 610, 784, 1.5)
    add_line("accentPrimary", 782, 750, 782, 660, 1.5)

    children[#children + 1] = ui.gradientPanel { gradient = "homeVioletBorder", color = "bgPanel", x = X(856), y = Y(186),
        width = S(488), height = S(612), cutLeft = S(20), cutRight = S(28), cutY = S(34), absolute = true,
        thickness = math.max(1.5, S(2)), glow = 0.62 * pulse }
    children[#children + 1] = ui.panel { border = "borderVioletDim", x = X(870), y = Y(200),
        width = S(460), height = S(584), cutLeft = S(20), cutRight = S(28), cutY = S(34),
        fill = false, absolute = true, thickness = math.max(1, S(1)) }
    -- Right panel inner accent highlights
    add_line("accentSecondary", 890, 200, 1040, 200, 2)
    add_line("accentSecondary", 870, 234, 870, 310, 2)
    add_line("accentTertiary", 1302, 784, 1160, 784, 1.5)
    add_line("accentTertiary", 1330, 750, 1330, 670, 1.5)

    add_gradient_line("homeBridge", 800, 479, 856, 479, 2, 0.75 * pulse, 0)
    -- Bridge connector dots
    children[#children + 1] = rect("accentPrimary", X(797), Y(476), S(6), S(6), S(3), 0.52)
    children[#children + 1] = rect("accentSecondary", X(853), Y(476), S(6), S(6), S(3), 0.52)

    -- Left: MEDIA SOURCE.
    children[#children + 1] = text("01 / MEDIA SOURCE", "accentPrimary", S(11), X(150), Y(210), S(596), S(20), "left")
    children[#children + 1] = text("Select or stream your media file", "textPanelDescription", S(12), X(150), Y(232), S(596), S(20), "left")
    -- OPEN LOCAL FILE button with leading dot decoration
    children[#children + 1] = rect("accentPrimary", X(159), Y(291), S(8), S(8), S(4), 0.85)
    children[#children + 1] = ui.button { text = ">  OPEN LOCAL FILE", action = "openLocalFile", theme = "primary", background = "bgLocalButton",
        x = X(150), y = Y(282), width = S(596), height = S(52), size = F(15), rounding = S(3), absolute = true }
    children[#children + 1] = ui.rect { color = "accentPrimary", x = X(150), y = Y(282), width = S(596), height = S(52),
        rounding = S(3), opacity = 0.08 * pulse, absolute = true }
    children[#children + 1] = ui.rect { color = "accentPrimary", x = X(150), y = Y(282), width = S(596), height = S(52),
        rounding = S(3), opacity = 0.24 * pulse, outline = true, thickness = math.max(1, S(1.5)), absolute = true }
    children[#children + 1] = center_text("or drag & drop a file here", "textMuted", S(11), X(150), Y(338), S(580), S(22))
    -- MERGE VIDEOS button with leading dot decoration
    children[#children + 1] = rect("accentSecondary", X(159), Y(385), S(8), S(8), S(4), 0.85)
    children[#children + 1] = ui.button { text = "+  MERGE VIDEOS", action = "openMerge", theme = "secondary", background = "bgMergeButton",
        x = X(150), y = Y(376), width = S(596), height = S(52), size = F(15), rounding = S(3), absolute = true }
    add_line("lineCyanHalf", 168, 450, 712, 450, 1)
    children[#children + 1] = center_text("02 / NETWORK STREAM", "textSection", S(10), X(168), Y(458), S(544), S(22))
    children[#children + 1] = ui.input { id = "url", stateKey = "url", value = state.get("url") or "",
        placeholder = "rtsp://... or https://...", x = X(150), y = Y(490), width = S(500), height = S(38), size = F(11),
        rounding = S(3), background = "bgField", border = "accentPrimary", borderOpacity = 0.40, textColor = "textFieldHint", absolute = true }
    children[#children + 1] = ui.button { text = "OPEN URL", action = "playUrl", payload = { url = state.get("url") or "" },
        theme = "secondary", background = "accentSecondary", backgroundOpacity = 0.08,
        hoverBackgroundOpacity = 0.16, hoverTextColor = "textPrimary",
        x = X(660), y = Y(490), width = S(86), height = S(38), size = F(12), rounding = S(3), absolute = true }

    -- Settings gear icon using new gearIcon widget (SVG line 151-155): 38x38 box centered at x=425, y=560.5
    -- Background box
    children[#children + 1] = rect("accentPrimary", X(406), Y(540), S(38), S(38), S(3), 0.06)
    children[#children + 1] = ui.rect { color = "accentPrimary", x = X(406), y = Y(540), width = S(38), height = S(38),
        rounding = S(3), opacity = 1, outline = true, thickness = S(1.5), absolute = true }
    -- Gear icon
    children[#children + 1] = ui.gearIcon { centerX = X(425), centerY = Y(560.5), size = S(38),
        color = "accentPrimary", opacity = 0.85, absolute = true }
    -- Clickable button overlay (transparent)
    children[#children + 1] = ui.button { text = "", action = "openSettings", theme = "primary",
        x = X(406), y = Y(540), width = S(38), height = S(38), size = F(10), rounding = S(3), absolute = true }
    add_line("lineCyanHalf", 168, 600, 712, 600, 1)
    children[#children + 1] = center_text("03 / HARDWARE INFO", "textSection", S(10), X(168), Y(606), S(544), S(24))
    children[#children + 1] = rect("bgField", X(150), Y(640), S(596), S(92), S(3), 0.85)

    -- Line 1: Decoder with status indicator
    children[#children + 1] = text("解码器：", "textMuted", S(10), X(164), Y(646), S(76), S(20), "left")
    children[#children + 1] = text(hardware.decoder or "Software decode", "accentPrimary", S(10), X(240), Y(646), S(290), S(20), "left")
    if hardware.enabled then
        children[#children + 1] = rect("stateSuccess", X(530), Y(653), S(5), S(5), S(2.5), 1)
        children[#children + 1] = text("已启用", "stateSuccess", S(9), X(540), Y(646), S(50), S(20), "left")
    end

    -- Line 2: Hardware device
    children[#children + 1] = text("硬件设备：", "textMuted", S(10), X(164), Y(666), S(76), S(20), "left")
    children[#children + 1] = text(hardware.device or "Unknown", "accentSecondary", S(10), X(240), Y(666), S(400), S(20), "left")

    -- Line 3: Performance tier with progress bar
    children[#children + 1] = text("性能档位：", "textMuted", S(10), X(164), Y(686), S(76), S(20), "left")
    children[#children + 1] = text(hardware.tier or "Unknown", "textPrimary", S(10), X(240), Y(686), S(90), S(20), "left")
    local tierPercent = tonumber(hardware.tierPercent) or 80
    children[#children + 1] = rect("accentPrimary", X(330), Y(692), S(80), S(12), S(2), 0.15)
    children[#children + 1] = rect("accentPrimary", X(330), Y(692), S(80 * tierPercent / 100), S(12), S(2), 0.45)
    children[#children + 1] = center_text(tierPercent .. "%", "textPrimary", S(9), X(330), Y(688), S(80), S(16))

    -- Line 4: Max speed support
    children[#children + 1] = text("播放倍数：", "textMuted", S(10), X(164), Y(706), S(76), S(20), "left")
    children[#children + 1] = text("1080p → " .. (hardware.speed1080 or "2") .. "x", "accentPrimary", S(10), X(240), Y(706), S(100), S(20), "left")
    children[#children + 1] = text("|", "textMuted", S(10), X(340), Y(706), S(20), S(20), "center")
    children[#children + 1] = text("4K → " .. (hardware.speed4k or "2") .. "x", "accentSecondary", S(10), X(360), Y(706), S(90), S(20), "left")

    children[#children + 1] = center_text("MP4  MKV  AVI  MOV  FLV  WebM  RTSP  RTMP  HTTP  HLS", "textSection", S(10), X(150), Y(738), S(596), S(24))

    -- Right: WATCH HISTORY.
    children[#children + 1] = text("WATCH HISTORY", "accentSecondary", S(11), X(898), Y(210), S(414), S(20), "left")
    add_line("borderVioletDim", 898, 236, 1312, 236, 1)
    local rowY = 266
    local maxRows = math.min(#history, 9)
    for i = 1, maxRows do
        local entry = history[i]
        -- Fade out effect for items beyond 3
        local titleColor = i <= 3 and "textHistoryTitle" or "textHistoryDisabled"

        -- Row number in muted color with sequence formatting (x=898)
        children[#children + 1] = text(string.format("%02d", i), i <= 3 and "textFieldHint" or "textDisabled", S(9), X(898), Y(rowY), S(20), S(20), "left")
        -- Title text - manually truncate to prevent overflow
        local titleText = "> " .. (entry.title or entry.path)
        if #titleText > 52 then
            titleText = titleText:sub(1, 49) .. "..."
        end
        children[#children + 1] = text(titleText, titleColor, S(12), X(918), Y(rowY), S(360), S(20), "left")
        -- Invisible button for title click area
        children[#children + 1] = ui.button { text = "", action = "replayHistory", payload = { path = entry.path or "" },
            theme = "primary", x = X(918), y = Y(rowY - 2), width = S(360), height = S(24), size = F(12), rounding = 0, absolute = true }
        -- Delete button (×) at x=1322 right-aligned
        children[#children + 1] = text("×", "stateError", S(11), X(1300), Y(rowY), S(22), S(20), "right")
        -- Invisible button for delete click area
        children[#children + 1] = ui.button { text = "", action = "removeHistory", payload = { id = entry.id or "" },
            theme = "danger", x = X(1300), y = Y(rowY - 2), width = S(22), height = S(24), size = F(11), rounding = 0, absolute = true }
        -- Time and source info on second line (x=918)
        children[#children + 1] = text((entry.duration or "--:--") .. "  ·  " .. (entry.source or "local"),
            i <= 3 and "textFieldHint" or "textDisabled", S(10), X(918), Y(rowY + 18), S(370), S(18), "left")
        -- Separator line - using dim color matching SVG's #2A3E54 (line 218, 224, 230)
        add_line("textSeparator", 898, rowY + 48, 1312, rowY + 48, 1)
        rowY = rowY + 52
    end
    if #history == 0 then
        children[#children + 1] = center_text("No history yet", "textMuted", S(11), X(898), Y(360), S(414), S(24))
    end
    children[#children + 1] = ui.button { text = "CLEAR ALL", action = "clearHistory", theme = "secondary",
        background = "accentSecondary", backgroundOpacity = 0.08, hoverBackgroundOpacity = 0.16,
        borderOpacity = 0.60, hoverTextColor = "textPrimary",
        x = X(898), y = Y(740), width = S(414), height = S(32), size = F(12), rounding = S(3), absolute = true }

    add_gradient_line("homeRail", 102, 862, 1338, 862, 1.2, 0.55 * pulse, 0)
    children[#children + 1] = text("LOCAL MEDIA", "textFooter", S(9), X(102), Y(868), S(180), S(18), "left")
    children[#children + 1] = text("NETWORK STREAM / WEB VIDEO", "textFooter", S(9), X(1110), Y(868), S(228), S(18), "right")

    return ui.container { layout = "absolute", width = width, height = height, table.unpack(children) }
end

local function format_clock(raw)
    local seconds = math.max(0, tonumber(raw) or 0)
    local minutes = math.floor(seconds / 60)
    return string.format("%02d:%02d:%02d", math.floor(seconds/3600), minutes%60, math.floor(seconds%60))
end

-- SVG-space drawing helpers. All geometry and appearance are Lua-owned;
-- sliders submit an invisible hit area separately from their artwork.
-- 设置项的候选项由 C++ 以 "value\tlabel\tvalue\tlabel…" 一行下发。
-- 用制表符而非换行：数据通道是扁平的字符串表，制表符在 Lua 侧最好切。
local function splitTabs(text)
    local out = {}
    for piece in string.gmatch(text or "", "[^\t]+") do out[#out + 1] = piece end
    return out
end

local function optionsFromRow(row)
    local tokens = splitTabs(row.options)
    local opts = {}
    for i = 1, #tokens, 2 do
        local value, label = tokens[i], tokens[i + 1] or tokens[i]
        if value and value ~= "" then opts[#opts + 1] = { value = value, label = label } end
    end
    return opts
end

local function canvas(width, height, X, Y, S, textFont)
    local c, out = {}, {}
    function c.add(kind, x, y, w, h, props)
        props = props or {}
        props.x, props.y, props.width, props.height = X(x), Y(y), S(w), S(h)
        props.absolute = true
        out[#out + 1] = ui[kind](props)
    end
    function c.rect(x, y, w, h, color, alpha, outline, radius, thickness)
        c.add("rect", x, y, w, h, { color = color, opacity = alpha or 1,
            outline = outline or false, rounding = S(radius or 3), thickness = S(thickness or 1) })
    end
    function c.text(value, x, baseline, w, size, color, align)
        local finalSize = S(size)
        c.add("text", x, baseline - size, w, size * 1.25, { content = tostring(value or ""),
            size = finalSize, font = textFont or "body",
            baseline = textFont and S(size) or nil,
            color = color or "textSecondary", textAlign = align or "left", ellipsis = true })
    end
    function c.line(x, y, x2, y2, color, thickness, opacity)
        c.add("line", x, y, math.abs(x2-x), math.abs(y2-y), { x2 = S(x2-x), y2 = S(y2-y),
            color = color, thickness = S(thickness or 1), opacity = opacity or 1 })
    end
    function c.rail(x, y, w, role, thickness)
        c.add("gradientLine", x, y, w, 2, { x2 = S(w), y2 = 0, gradient = role,
            thickness = S(thickness or 2) })
    end
    function c.dot(x, y, r, color, alpha)
        c.rect(x-r, y-r, r*2, r*2, color, alpha, false, r)
    end
    function c.hit(action, payload, x, y, w, h)
        c.add("button", x, y, w, h, { text = "", action = action, payload = payload })
    end
    function c.button(label, action, x, y, w, h, color, payload, alpha, size)
        color = color or "accentPrimary"
        local mouse = ui.getInput()
        local hovered = mouse.x >= X(x) and mouse.x <= X(x+w) and mouse.y >= Y(y) and mouse.y <= Y(y+h)
        c.rect(x, y, w, h, color, hovered and 0.16 or (alpha or 0.04))
        c.rect(x, y, w, h, color, 0.68, true)
        c.text(label, x+3, y+h/2+(size or 12)*0.36, w-6, size or 12, color, "center")
        c.hit(action, payload, x, y, w, h)
    end
    -- action 可选：设置面板里的滑块需要发 configChanged，播放器的 seek/音量则用默认事件
    function c.slider(id, value, min, max, x, y, w, h, color, thin, action)
        local t = max > min and math.max(0, math.min(1, (value-min)/(max-min))) or 0
        c.rect(x, y, w, h, "bgField", 1, false, 2)
        if thin then
            c.line(x+6, y+h/2, x+w-6, y+h/2, "accentPrimaryDim", 2)
            c.line(x+6, y+h/2, x+6+(w-12)*t, y+h/2, color, 2.5)
        else
            c.rect(x, y, w, h, color, 0.4, true, 2)
            c.rect(x, y, w*t, h, color, 0.25, false, 2)
        end
        c.dot(x+(thin and (6+(w-12)*t) or w*t), y+h/2, 5, color)
        c.add("slider", x, y, w, h, { id = id, value = value, min = min, max = max,
                                      invisible = true, action = action })
    end
    function c.finish() return ui.container { layout = "absolute", width = width, height = height, table.unpack(out) } end
    return c
end

-- Keep Player controls at the SVG's logical size on large windows; only shrink to fit.
-- Subtitles share this transform so their clearance above the dock stays consistent.
local function player_scale(width, height)
    return math.min(1, width/1440, height/900)
end

local function render_player()
    local width, height = ui.getWindowSize()
    local scale = player_scale(width, height)
    local S = function(v) return v*scale end
    local X = function(v) return (width-S(1440))/2+S(v) end
    local Y = function(v) return height-S(900-v) end
    local anchor = 0
    local c = canvas(width, height, function(v) return anchor+S(v) end, Y, S)
    local p = (ui.getData("player") or {})[1] or {}
    local stats = (ui.getData("statistics") or {})[1] or {}
    local info = (ui.getData("mediaInfo") or {})[1] or {}
    local n = function(v) return tonumber(v) or 0 end
    -- Edge-anchored groups keep SVG insets; only the seek track stretches.
    local dockW = width/scale
    c.rect(0, 812, dockW, 88, "#070D22", 0.92, false, 0)
    c.rail(0, 813, dockW, "dockEdge", 2)
    -- 8px gap, a 192px time column, and an 8px right inset.
    local seekW = dockW-216
    local progress = math.max(0, math.min(1, n(p.progress)))
    c.rect(8, 819, seekW, 16, "bgPanelRaised", 1, false, 2)
    c.rect(8, 819, seekW, 16, "lineSubtle", 1, true, 2)
    c.add("rectGradient", 8, 819, seekW*progress, 16, { colorTop = "accentPrimary", colorBottom = "accentSecondary", direction = "horizontal" })
    c.dot(8+seekW*progress, 827, 12, "accentPrimary", 0.08)
    c.dot(8+seekW*progress, 827, 8, "accentPrimary", 0.22)
    c.dot(8+seekW*progress, 827, 5, "accentPrimary")
    c.add("slider", 8, 819, seekW, 16, { id = "playerSeek", value = progress, min = 0, max = 1, invisible = true })
    c.add("text", dockW-200, 819, 192, 16, {
        content = format_clock(p.current).." / "..format_clock(p.duration),
        size = 12, baseline = S(12), font = "mono", color = "textPrimary", textAlign = "right" })
    if p.network == "true" then c.button("Download", "download", 8, 855, 72, 38, "accentSecondary") end
    if p.downloading == "true" then
        local dp = math.max(0, math.min(1, n(p.downloadProgress)))
        c.rect(88, 855, 120, 38, "bgPanelRaised")
        c.add("rectGradient", 88, 855, 120*dp, 38, { colorTop = "accentPrimary", colorBottom = "accentSecondary", direction = "horizontal", opacity = 0.26 })
        -- 阶段文案与 C++ 版本一致：实时保存 / 探测中 / 百分比
        local label = p.downloadMode == "LIVE" and "LIVE SAVE"
                   or (dp <= 0 and "PROBING" or string.format("%d%% · VOD", math.floor(dp*100)))
        c.text(label, 88, 879, 120, 12, "textPrimary", "center")
        c.button(p.downloadPaused == "true" and ">" or "II", "toggleDownloadPause", 212, 855, 24, 38)
        c.button("×", "cancelDownload", 240, 855, 24, 38, "stateError")
        c.text(p.downloadRate or "", 270, 869, 150, 12, "accentPrimary")
        c.text(p.downloadDetail or "", 270, 888, 150, 11, "accentPrimary")
        -- 第二行补上中转链路：remux 不经过像素帧，必须显示 BYPASS/N/A 而不是硬件零拷贝
        local chain = "D:"..(p.downloadDecoder or "").." E:"..(p.downloadEncoder or "")
                   .." ZC:"..(p.downloadZeroCopy or "")
        if (p.downloadDecoder or "") ~= "" then
            c.text(chain, 424, 879, 200, 10, "textMuted")
        end
    end
    if stats.hardware == "true" then
        c.rect(427, 855, 38, 17, "accentPrimary", 0.12)
        c.rect(427, 855, 38, 17, "accentPrimary", 0.48, true)
        c.text("HW D", 427, 867, 38, 9, "accentPrimary", "center")
    end
    -- Playback/recording currently remuxes packets, so no hardware encoder is active.
    -- Keep the design's encoder status chip visible without implying HW encoding.
    c.rect(469, 855, 38, 17, "accentSecondary", 0.05)
    c.rect(469, 855, 38, 17, "accentSecondary", 0.25, true)
    c.text("HW E", 469, 867, 38, 9, "textMuted", "center")
    if stats.zeroCopy == "true" then
        c.rect(427, 876, 80, 17, "accentPrimary", 0.12)
        c.rect(427, 876, 80, 17, "accentPrimary", 0.48, true)
        c.dot(436, 884.5, 3, "accentPrimary")
        c.text("ZERO-COPY", 443, 888, 61, 9, "accentPrimary", "center")
    end
    anchor = (width-S(1440))/2
    c.button("", "togglePlayback", 580, 855, 80, 38)
    if p.state == "playing" then
        c.line(616,866,616,882,"accentPrimary",3); c.line(624,866,624,882,"accentPrimary",3)
    else
        c.line(615,866,627,874,"accentPrimary",3); c.line(627,874,615,882,"accentPrimary",3); c.line(615,882,615,866,"accentPrimary",3)
    end
    c.button("", "stop", 668, 855, 60, 38, "accentSecondary")
    c.rect(693,867,11,11,"accentSecondary",1,false,0)
    c.button(p.recordingVideo == "true" and "* REC V" or "REC V", "toggleVideoRecording", 736,855,70,38,"stateError",nil,p.recordingVideo == "true" and 0.18 or 0.04)
    c.button(p.recordingAudio == "true" and "* REC A" or "REC A", "toggleAudioRecording", 814,855,60,38)
    -- 录制计时器：C++ 只给秒数与字节数，排版由皮肤决定（原来这行文字在 C++ 里拼）
    if p.recordingVideo == "true" or p.recordingAudio == "true" then
        local function recLabel(prefix, seconds, bytes)
            local total=math.floor(n(seconds))
            local size=n(bytes)
            local amount = size < 1024*1024
                and string.format("%.0fKB", size/1024)
                or  string.format("%.1fMB", size/(1024*1024))
            return string.format("%s %02d:%02d %s", prefix, math.floor(total/60), total%60, amount)
        end
        local parts={}
        if p.recordingVideo == "true" then
            parts[#parts+1]=recLabel("V", p.recordingVideoTime, p.recordingVideoSize)
        end
        if p.recordingAudio == "true" then
            parts[#parts+1]=recLabel("A", p.recordingAudioTime, p.recordingAudioSize)
        end
        c.text(table.concat(parts," | "),884,879,180,11,"stateError")
    end
    -- HUD toggles occupy the otherwise empty dock gap, not an extra row.
    c.button("INFO", "toggleMediaInfo", 918,855,60,38)
    c.button("STATS", "toggleStats", 986,855,60,38,"accentSecondary")
    -- Right tools and their popups move together without stretching buttons/icons.
    anchor = width-S(1440)
    c.button(p.quality ~= "" and p.quality or "Quality", "setUiState",1090,855,60,38,nil,{key="qualityMenu",value=state.get("qualityMenu")=="true" and "false" or "true"})
    c.button(n(p.speed)==1 and "Speed" or string.format("%gx", n(p.speed)), "setUiState",1154,855,60,38,nil,{key="speedMenu",value=state.get("speedMenu")=="true" and "false" or "true"})
    if state.get("speedMenu") == "true" then
        -- Refresh from the native capability estimate every frame, including while open.
        local maxSpeed = tonumber(p.maxSpeed) or 1
        local speeds = {}
        for _, speed in ipairs({0.5,0.75,1,1.25,1.5,2,4,8,16}) do
            if speed <= maxSpeed then speeds[#speeds + 1] = speed end
        end
        local menuHeight = #speeds * 26 + 4
        c.rect(1136,847-menuHeight,68,menuHeight,"bgPanelRaised",0.96,false,4)
        c.rect(1136,847-menuHeight,68,menuHeight,"accentPrimary",0.68,true,4)
        for i, speed in ipairs(speeds) do
            c.button(string.format("%gx",speed),"setSpeed",1140,847-i*26,60,24,nil,
                {value=tostring(speed)},math.abs(n(p.speed)-speed)<0.01 and 0.18 or 0.04)
        end
    end
    if state.get("qualityMenu") == "true" then
        local qualities = ui.getData("qualities") or {}
        if #qualities == 0 then c.text("Source quality",1050,842,150,11,"textMuted") end
        for i, quality in ipairs(qualities) do
            c.button(quality.label,"setQuality",1050,847-i*28,100,26,nil,{index=quality.index})
        end
    end
    c.button("", "setUiState",1218,855,26,38,nil,{key="brightnessPopup",value=state.get("brightnessPopup")=="true" and "false" or "true"})
    c.rect(1227,870,8,8,"accentPrimary",1,true,4)
    for i=0,7 do local a=i*math.pi/4; c.line(1231+8*math.cos(a),874+8*math.sin(a),1231+11*math.cos(a),874+11*math.sin(a),"accentPrimary",1.5) end
    if state.get("brightnessPopup") == "true" then
        c.rect(1206,685,50,162,"bgPanelRaised",0.96, false,4)
        c.rect(1206,685,50,162,"accentPrimary",0.68,true,4)
        local by=812-(math.max(0.25,math.min(2,n(p.brightness)))-0.25)/1.75*112
        c.line(1231,700,1231,812,"accentPrimaryDim",3)
        c.line(1231,by,1231,812,"accentPrimary",3); c.dot(1231,by,6,"accentPrimary")
        c.add("slider",1215,700,32,112,{id="playerBrightness",value=n(p.brightness),min=0.25,max=2,vertical=true,invisible=true})
        c.text(string.format("%d%%",math.floor(n(p.brightness)*100+0.5)),1206,835,50,10,"textPrimary","center")
    end
    c.button("", "openSettings",1248,855,26,38)
    -- Eight teeth, defined here rather than a backend-specific gear drawing.
    for i=0,7 do local a=i*math.pi/4; c.line(1261+8*math.cos(a),874.5+8*math.sin(a),1261+12*math.cos(a),874.5+12*math.sin(a),"accentPrimary",5) end
    c.rect(1253,866.5,16,16,"accentPrimary",1,true,8,3)
    c.dot(1261,874.5,3.1,"accentPrimary"); c.dot(1261,874.5,1.45,"bgVoid")
    c.button("", "toggleMute",1278,855,26,38)
    c.rect(1284,871,5,6,"accentPrimary",1,false,0)
    c.line(1289,871,1294,867,"accentPrimary",2); c.line(1294,867,1294,881,"accentPrimary",2); c.line(1294,881,1289,877,"accentPrimary",2)
    c.line(1297,869,1300,874,"accentPrimary",1); c.line(1300,874,1297,879,"accentPrimary",1)
    if p.muted == "true" then c.line(1282,864,1301,884,"stateError",2) end
    c.slider("playerVolume",n(p.volume),0,1,1308,855,120,38,"accentPrimary",true)

    -- HUD coordinates are anchored to the top, independent of the dock's bottom anchor.
    local function hudX(v)
        if v < 720 then return S(v) end
        return width-S(1440-v)
    end
    local hud = canvas(width,height,hudX,function(v) return S(v) end,S,"mono")
    local hudText = "#FFFFFF"
    local function panel(x,w,h,title,color,action)
        hud.rect(x,10,w,h,"#070D22",0.96,false,6)
        hud.rect(x,10,w,27,"bgPanelRaised",1,false,6)
        hud.rect(x,10,w,h,color,1,true,6,2)
        hud.text(title,x+12,29,w-40,12,hudText)
        hud.text("x",x+w-22,29,14,12,hudText); hud.hit(action,nil,x+w-28,10,28,27)
    end
    if p.showMediaInfo == "true" then
        local web = (info.platform or "") ~= "" or (info.uploader or "") ~= ""
        panel(10,450,web and 405 or 293,"Media Info","accentPrimary","toggleMediaInfo")
        hud.text("FILE: "..(info.filename or ""),24,59,424,12,hudText)
        hud.line(22,69,448,69,"lineSubtle")
        local y=91
        if web then
            hud.text("WEB VIDEO:",24,y,424,12,hudText)
            for i,row in ipairs({"Platform   : "..(info.platform or ""),"Uploader   : "..(info.uploader or ""),"Views      : "..(info.views or ""),"Upload Date: "..(info.uploadDate or "")}) do hud.text(row,42,y+20+(i-1)*19,406,12,hudText) end
            hud.line(22,182,448,182,"lineSubtle"); y=204
        end
        hud.text("VIDEO:",24,y,424,12,hudText)
        local rows={"Resolution : "..(info.width or "0").."x"..(info.height or "0"),"Codec      : "..(info.videoCodec or "").." ("..(info.videoProfile or "")..")",string.format("FPS        : %.2f",n(info.fps)),string.format("GOP        : %d frames (%.2f sec)",n(info.gop),n(info.fps)>0 and n(info.gop)/n(info.fps) or 0)}
        for i,row in ipairs(rows) do hud.text(row,42,y+19*i,406,12,hudText) end
        hud.line(22,y+90,448,y+90,"lineSubtle")
        hud.text("AUDIO:",24,y+112,424,12,hudText)
        for i,row in ipairs({"Codec      : "..(info.audioCodec or "").." ("..(info.audioProfile or "")..")","Sample Rate: "..(info.sampleRate or "0").." Hz","Channels   : "..(info.channels or "0").." ("..(info.channelLayout or "")..")"}) do hud.text(row,42,y+112+19*i,406,12,hudText) end
        hud.text("Duration   : "..format_clock(p.duration),24,y+189,424,12,hudText)
    end
    if p.showStats == "true" then
        panel(1030,400,348,"Statistics","accentSecondary","toggleStats")
        hud.text("PERFORMANCE:",1044,59,374,12,hudText)
        hud.text(string.format("FPS        : %.0f",n(stats.fps)),1060,80,358,12,hudText)
        hud.text(string.format("Bitrate    : %.2f Mbps",n(stats.bitrate)),1060,99,358,12,hudText)
        hud.text("Dropped    : "..(stats.dropped or "0"),1060,118,358,12,hudText)
        hud.line(1042,130,1418,130,"accentSecondary",1,0.3)
        hud.text("BUFFER:",1044,152,374,12,hudText)
        hud.text("Video : "..(stats.videoQueue or "0").." frames",1060,171,358,12,hudText)
        hud.text("Audio : "..(stats.audioQueue or "0").." frames",1060,190,358,12,hudText)
        hud.line(1042,202,1418,202,"accentSecondary",1,0.3)
        hud.text("VIDEO PIPELINE:",1044,224,374,12,hudText)
        for i,row in ipairs({"Decoder   : "..(stats.hardware=="true" and "HARDWARE" or "SOFTWARE"),"Backend   : "..(stats.backend or ""),"Device    : "..(stats.device or ""),"Zero-copy : "..(stats.zeroCopy=="true" and "ACTIVE" or "INACTIVE"),"Path      : "..(stats.path or "")}) do hud.text(row,1060,224+19*i,358,12,hudText) end
        hud.text("STATE : "..string.upper(p.state or ""),1044,339,374,12,hudText)
    end
    return ui.container { layout="absolute",width=width,height=height,c.finish(),hud.finish() }
end


local function render_merge()
    local width,height=ui.getWindowSize()
    local _,X,Y,S=viewport(width,height)
    local c=canvas(width,height,X,Y,S,"mono")
    local status=(ui.getData("merge") or {})[1] or {}
    local clips=ui.getData("mergeClips") or {}
    local n=function(v) return tonumber(v) or 0 end
    local ms=function(v) local t=math.max(0,n(v)); return string.format("%02d:%02d.%03d",math.floor(t/60),math.floor(t%60),math.floor(t*1000)%1000) end
    -- 与 Controller::renderMediaInfo 的容量口径保持一致(1024 进制,低于 0.1GB 降级为 MB),
    -- 避免小文件被 "%.1f GB" 统一压成 0.0 GB。
    local bytes=function(v)
        local b=n(v)
        if b >= 107374182.4 then return string.format("%.1f GB",b/1073741824) end
        if b >= 1048576 then return string.format("%.1f MB",b/1048576) end
        if b > 0 then return string.format("%.0f KB",b/1024) end
        return "--"
    end
    c.add("radialGradient",0,0,1440,900,{colorCenter="bgRadialCenter",colorMiddle="bgRadialMiddle",colorOuter="bgRadialOuter",cx=0.5,cy=0.5,radiusRatio=0.7})
    c.rail(0,2,1440,"homeRail",2.5); c.rail(0,898,1440,"homeRail",2.5)
    for _,p in ipairs({{28,28,84,100,44},{1412,28,1356,1340,44},{28,872,84,100,856},{1412,872,1356,1340,856}}) do
        c.line(p[1],p[2],p[3],p[2],"accentPrimaryDim",1.5); c.line(p[3],p[2],p[4],p[5],"accentPrimaryDim",1.5)
    end
    for _,y in ipairs({58,842}) do c.line(28,y,66,y,"accentSecondary",1.5,0.48); c.line(1374,y,1412,y,"accentSecondary",1.5,0.48) end
    for _,p in ipairs({{220,130,100,40},{1220,130,1340,40},{220,770,80,860},{1220,770,1360,860}}) do c.line(p[1],p[2],p[3],p[4],"accentSecondary",1,0.06) end
    for _,p in ipairs({{160,85},{1280,85},{130,815},{1310,815}}) do c.dot(p[1],p[2],2,"accentSecondary",0.18) end
    c.text("FLUX / MERGE TOOL",48,22,600,9,"textTopStatus")
    c.text(status.phase=="editing" and "READY" or string.upper(status.phase or "READY"),1100,22,292,9,"textTopStatus","right")
    c.add("text",404,34,640,60,{content="M E R G E  V I D E O S",size=S(48),color="accentSecondary",opacity=0.30})
    c.add("text",396,30,640,60,{content="M E R G E  V I D E O S",size=S(48),color="accentPrimary",opacity=0.24})
    c.add("text",400,32,640,60,{content="M E R G E  V I D E O S",size=S(48),color="textPrimary"})
    c.text("C O N C A T E N A T E  M U L T I P L E  F I L E S  I N T O  O N E",360,105,720,10,"#7792B2","center")
    c.rail(450,118,160,"homeEnergy",1.5); c.rail(830,118,160,"homeEnergy",1.5); c.dot(720,118,3,"accentSecondary")
    -- 编辑态与合并态共用 1000x640 宽面板：两者内容都在面板内排布。
    -- 结果态内容少得多，沿用同一块边框会留下一大片空白，所以单独收窄成居中卡片。
    local editing = (status.phase or "editing") == "editing"
    local mergingNow = (status.phase or "editing") == "merging"
    if editing or mergingNow then
        c.add("gradientPanel",220,130,1000,640,{color="bgPanel",gradient="panelHeader",cut=S(24),thickness=S(2)})
        c.add("panel",234,144,972,612,{fill=false,border="borderVioletDim",cut=S(24),thickness=S(1)})
        c.line(258,144,440,144,"accentSecondary",2); c.line(234,168,234,260,"accentSecondary",2)
        c.line(1182,756,1000,756,"accentPrimary",1.5,0.5); c.line(1206,732,1206,640,"accentPrimary",1.5,0.5)
        c.rail(220,750,1000,"homeVioletBorder",3)
    end
    c.text("VIDEO MERGE TOOL",102,882,400,9,"textFooter")
    c.text("MP4 · MKV · AVI · MOV · FLV",980,882,358,9,"textFooter","right")
    c.rail(102,862,1236,"homeRail",1.2)

    -- 合并态：与编辑态共用 1000x640 宽面板（上方 842-848 行已绘制）。
    -- 几何与 source/UI/skins/cyberpunk-neon/mockup_merge_merging.svg 一一对应。
    -- 链路信息字段来自 MergeScreen::provideLuaData("merge") 的
    -- transcoded / hwDeviceType / hwDecoding / decoderName / hwEncoding / encoderName / zeroCopy。
    if (status.phase or "editing") == "merging" then
        c.text(string.format("MERGING... %d%%", math.floor(n(status.progress) * 100 + 0.5)),
               268, 330, 904, 30, "accentPrimary", "center")
        -- 不放 resultHint：startMerge() 已清空它，合并期间恒为空。

        -- 进度条收在内容区中部，与下方链路信息同宽，不横贯整块面板。
        local barX, barW = 480, 480
        local pct = math.max(0, math.min(1, n(status.progress)))
        c.rect(barX, 455, barW, 8, "bgPanelRaised")
        c.rect(barX, 455, barW * pct, 8, "accentPrimary")

        -- 链路信息：即使回退软件也要显示，不能留空白
        local transcode = status.transcoded == "true"
        local device = not transcode and "Stream Copy"
            or ((status.hwDeviceType or "") ~= "" and status.hwDeviceType or "CPU / Software")
        c.text("Merge Pipeline", 268, 505, 904, 11, "textPrimary", "center")
        c.text(device, 268, 528, 904, 12, "accentPrimary", "center")

        local dec = not transcode and "BYPASS"
            or ((status.hwDecoding == "true" and "HW " or "SW ") ..
                ((status.decoderName or "") ~= "" and "(" .. status.decoderName .. ")" or "(initializing...)"))
        local enc = not transcode and "BYPASS"
            or ((status.hwEncoding == "true" and "HW " or "SW ") ..
                ((status.encoderName or "") ~= "" and "(" .. status.encoderName .. ")" or "(initializing...)"))
        c.text("Decode:", 548, 556, 120, 11, "textMuted")
        c.text(dec, 676, 556, 300, 11,
               status.hwDecoding == "true" and "accentSecondary" or "textSecondary")
        c.text("Encode:", 548, 580, 120, 11, "textMuted")
        c.text(enc, 676, 580, 300, 11,
               status.hwEncoding == "true" and "accentSecondary" or "textSecondary")

        local note = not transcode and "Packet remux (no decode / encode)"
            or (status.zeroCopy == "true" and "GPU Zero-Copy Pipeline"
                or "Software pipeline (CPU frames)")
        c.text(note, 268, 610, 904, 11,
               status.zeroCopy == "true" and "accentPrimary" or "textMuted", "center")

        -- 合并中不提供 BACK：合并在后台线程运行，此时退出语义模糊
        -- （C++ 版同样只在 Editing/Result 态给 BACK）。
        c.button("CANCEL", "mergeCancel", 480, 640, 480, 48, "accentSecondary")
        return c.finish()
    end

    -- 结果态：内容比编辑态少，收窄成贴合内容的居中卡片。
    if (status.phase or "editing") ~= "editing" then
        local done = (status.phase or "") == "done"

        local cardW = 520
        local cardH = 300
        local cardX = (1440 - cardW) / 2
        local cardY = (900 - cardH) / 2 - 20

        c.add("gradientPanel", cardX, cardY, cardW, cardH,
              { color = "bgPanel", gradient = "panelHeader", cut = S(18), thickness = S(2) })
        c.add("panel", cardX + 10, cardY + 10, cardW - 20, cardH - 20,
              { fill = false, border = "borderVioletDim", cut = S(14), thickness = S(1) })
        c.line(cardX + 26, cardY + 10, cardX + 150, cardY + 10, "accentSecondary", 2)
        c.line(cardX + 10, cardY + 32, cardX + 10, cardY + 96, "accentSecondary", 2)
        c.line(cardX + cardW - 26, cardY + cardH - 10, cardX + cardW - 150, cardY + cardH - 10,
               "accentPrimary", 1.5, 0.5)
        c.rail(cardX, cardY + cardH - 4, cardW, "homeVioletBorder", 3)

        c.text(done and "MERGE COMPLETE" or "MERGE FAILED", cardX, cardY + 52, cardW, 22,
               done and "stateSuccess" or "stateError", "center")

        local bodyY = cardY + 92
        c.text("Output file:", cardX, bodyY, cardW, 11, "textMuted", "center")
        local boxX, boxW = cardX + 32, cardW - 64
        c.rect(boxX, bodyY + 18, boxW, 32, "bgPanelRaised", 0.7, false, 3)
        c.rect(boxX, bodyY + 18, boxW, 32, "linePrimary", 0.35, true, 3)
        local shown = done and (status.resultPath or "") or (status.error or "")
        c.text(shown, boxX + 10, bodyY + 40, boxW - 20, 11,
               done and "textPrimary" or "stateError")
        if (status.resultHint or "") ~= "" then
            c.text(status.resultHint, cardX, bodyY + 70, cardW, 11, "textMuted", "center")
        end

        local btnY = cardY + cardH - 70
        c.button("MERGE AGAIN", "mergeAgain", cardX + 32, btnY, cardW - 64 - 104 - 12, 40,
                 "accentSecondary")
        c.button("BACK", "mergeBack", cardX + cardW - 32 - 104, btnY, 104, 40, "textMuted")
        return c.finish()
    end

    c.text("C L I P  L I S T",268,174,430,11,"accentSecondary")
    c.line(268,182,680,182,"accentSecondary",1,0.28)
    c.text("Select clips to trim · Drag to reorder",268,200,430,10,"#7792B2")
    c.text("C L I P  E D I T O R",730,174,442,11,"accentPrimary")
    c.line(730,182,1172,182,"accentPrimary",1,0.28)
    c.text("Trim IN / OUT points · Preview frames",730,200,442,10,"#7792B2")
    local selected,total=nil,0
    for _,clip in ipairs(clips) do if clip.selected=="true" then selected=clip end; total=total+n(clip.bytes) end
    local page=math.max(0,math.min(math.max(0,math.ceil(#clips/3)-1),n(state.get("mergePage"))))
    local mouse=ui.getInput()
    local hoverIndex=nil
    for row=1,3 do
        local i=page*3+row
        local clip=clips[i]
        if clip then
            local y=215+(row-1)*72
            c.rect(268,y,430,64,clip.selected=="true" and "#0A1E32" or "bgLocalButton")
            c.rect(268,y,430,64,"accentPrimary",clip.selected=="true" and 1 or 0.3,true,3,clip.selected=="true" and 1.5 or 1)
            c.text(string.format("%02d",i),288,y+21,24,10,"textMuted")
            c.text("> "..(clip.name or ""),312,y+21,348,12,clip.selected=="true" and "textPrimary" or "textSecondary")
            local trimmed=n(clip.startSec)>0 or n(clip.endSec)<n(clip.durationSec)
            c.text(trimmed and (ms(clip.startSec).." > "..ms(clip.endSec).."  [trimmed]") or "[full]",312,y+39,348,10,"textMuted")
            c.text((clip.resolution or "").." · "..(clip.codec or "").." · "..bytes(clip.bytes),312,y+53,348,9,"textMuted")
            c.hit("mergeSelect",{index=tostring(i)},268,y,394,64)
            c.text("×",670,y+33,20,16,"stateError")
            c.hit("mergeRemove",{index=tostring(i)},662,y,36,64)
            if mouse.x>=X(268) and mouse.x<X(662) and mouse.y>=Y(y) and mouse.y<Y(y+64) then hoverIndex=i end
        end
    end
    -- Gesture state lives in Lua; C++ only moves a clip in the timeline.
    if mouse.clicked and hoverIndex then state.set("mergeDrag",tostring(hoverIndex)) end
    if not mouse.down and state.get("mergeDrag") then
        local from=n(state.get("mergeDrag"))
        if from>0 and hoverIndex and from~=hoverIndex then events.emit("mergeMove",{index=tostring(from),to=tostring(hoverIndex)}) end
        state.set("mergeDrag","0")
    end
    if #clips==0 then c.text("Drop video files here",288,300,390,12,"textMuted","center") end
    if #clips>3 then
        c.button("<","setUiState",598,155,24,22,"textMuted",{key="mergePage",value=tostring(math.max(0,page-1))},0,10)
        c.text(string.format("%d/%d",page+1,math.ceil(#clips/3)),626,171,40,10,"textMuted","center")
        c.button(">","setUiState",674,155,24,22,"textMuted",{key="mergePage",value=tostring(page+1)},0,10)
    end
    c.rect(268,431,430,36,"accentPrimary",0.04)
    for x=268,697,8 do c.line(x,431,math.min(x+4,698),431,"accentPrimary",1.5); c.line(x,467,math.min(x+4,698),467,"accentPrimary",1.5) end
    for y=431,466,8 do c.line(268,y,268,math.min(y+4,467),"accentPrimary",1.5); c.line(698,y,698,math.min(y+4,467),"accentPrimary",1.5) end
    c.text("+  ADD FILES",268,453,430,12,"accentPrimary","center"); c.hit("mergeAddFiles",nil,268,431,430,36)
    c.text("Resolution:",268,490,430,10,"textMuted")
    local function radio(label,x,y,active,key,value,size)
        local color=active and "accentPrimary" or "#7792B2"
        c.rect(x-6,y-6,12,12,color,1,true,6,1.5)
        if active then c.dot(x,y,3,color) end
        c.text(label,x+12,y+4,130,size or 11,color)
        c.hit("mergeOption",{key=key,value=value},x-8,y-10,138,20)
    end
    radio("Keep Original",278,508,status.resolutionMode=="original","resolution","original")
    radio("Unified",420,508,status.resolutionMode~="original","resolution","unified")
    if status.resolutionMode~="original" then
        radio("First clip",294,530,status.firstClip=="true","firstClip","true",10)
        radio("Custom",294,550,status.firstClip~="true","firstClip","false",10)
        if status.firstClip~="true" then
            for _,field in ipairs({{"mergeWidth",360,60,status.customWidth},{"mergeHeight",445,60,status.customHeight},{"mergeGop",550,56,status.customGop}}) do
                c.add("input",field[2],542,field[3],20,{id=field[1],value=field[4],size=S(10),background="bgField",border="textMuted",color="textSecondary",rounding=S(2)})
            end
            c.text("x",427,555,16,10,"#7792B2"); c.text("GOP",520,555,30,10,"#7792B2")
        end
        -- 输出格式：视频 H.264/HEVC、音频 AAC/PCM。首项「Same as clip 1」= 不干预，
        -- 各源格式一致时仍走流拷贝；选具体格式则强制重编码（下方小字提示实际后果）。
        -- y 传「圆心」，与上方 radio() 保持同一约定（文字基线 = 圆心 + 4）。
        -- 若把 y 当圆的左上角，圆心会落到 y+6 而基线仍在 y+4，
        -- 圆圈就比自己那一行文字低约 6px，整行看着不在一条线上。
        local function formatRow(label,y,current,key,options)
            c.text(label,268,y+4,80,10,"textMuted")
            for _,opt in ipairs(options) do
                local active = current==opt.value
                local color = active and "accentPrimary" or "#7792B2"
                c.rect(opt.x-5,y-5,10,10,color,1,true,5,1.5)
                if active then c.dot(opt.x,y,2.5,color) end
                c.text(opt.text,opt.x+12,y+4,110,10,color)
                c.hit("mergeOption",{key=key,value=opt.value},opt.x-8,y-8,110,20)
            end
        end
        -- 圆心取自 mockup_merge.svg：Video 578 / Audio 602（行距 24）
        formatRow("Video:",578,status.videoCodec,"videoCodec",
            {{text="Same as clip 1",value="source",x=356},{text="H.264",value="h264",x=493},{text="HEVC",value="hevc",x=584}})
        formatRow("Audio:",602,status.audioCodec,"audioCodec",
            {{text="Same as clip 1",value="source",x=356},{text="AAC",value="aac",x=493},{text="PCM",value="pcm",x=584}})
        c.text(status.formatHint,268,625,430,9,"textMuted")
    else c.text("Mixed resolutions may be incompatible with other players.",268,548,430,10,"stateWarning") end
    c.rect(268,638,14,14,"accentPrimary",0.15,false,2); c.rect(268,638,14,14,"accentPrimary",1,true,2,1.5)
    if status.hardware=="true" then c.line(271,645,275,649,"accentPrimary",2); c.line(275,649,281,640,"accentPrimary",2) end
    c.text("Hardware Accel",288,649,410,11,"accentPrimary")
    c.hit("mergeOption",{key="hardware",value=status.hardware=="true" and "false" or "true"},268,632,180,26)
    c.button("S T A R T  M E R G E","mergeStart",268,668,430,44,"accentSecondary",nil,0.12,15)
    c.button("BACK","mergeBack",268,720,430,30,"#7792B2")
    c.text(status.error,268,756,430,10,"stateError")
    c.rect(730,215,442,249,"#020508")
    if selected and status.previewReady=="true" then
        local ratio=n(status.previewWidth)/math.max(1,n(status.previewHeight))
        local pw=math.min(442,249*ratio); local ph=pw/math.max(0.01,ratio)
        c.add("image",730+(442-pw)/2,215+(249-ph)/2,pw,ph,{path="texture:mergePreview"})
    else c.text(selected and (status.previewDecoding=="true" and "Decoding preview..." or "Preview unavailable") or "Select a clip",730,345,442,11,"textMuted","center") end
    c.rect(730,215,442,249,"accentPrimary",1,true,3,2)
    local edge=status.previewEdge=="OUT" and "accentSecondary" or "accentPrimary"
    c.line(732,217,762,217,edge,2.5); c.line(732,217,732,247,edge,2.5)
    c.text(status.previewEdge or "IN",738,233,100,9,edge)
    c.text(selected and selected.name or "",730,485,442,11,"textPrimary")
    if selected then
        c.text("IN",730,510,30,10,"accentPrimary")
        c.slider("mergeIn",n(selected.startSec),0,n(selected.durationSec),760,498,412,18,"accentPrimary")
        c.text(ms(selected.startSec),730,532,442,10,"#7792B2")
        c.text("OUT",730,560,30,10,"accentSecondary")
        c.slider("mergeOut",n(selected.endSec),0,n(selected.durationSec),760,548,412,18,"accentSecondary")
        c.text(ms(selected.endSec),730,582,442,10,"#7792B2")
        c.text("Duration: "..ms(n(selected.endSec)-n(selected.startSec)).." (from "..ms(selected.durationSec)..")",730,608,442,10,"textMuted")
        c.button("RESET","mergeReset",730,625,200,36,"#7792B2")
        c.button("DUPLICATE CLIP","mergeDuplicate",940,625,232,36)
    end
    c.text(string.format("Total: %d clips · %s · Output: record directory",#clips,bytes(total)),730,705,442,10,"textMuted")
    c.button("CANCEL","mergeBack",730,720,442,24,"#7792B2",nil,0.02,11)
    return c.finish()
end

skin.surfaces.subtitle.render = function()
    local width,height=ui.getWindowSize()
    local data=(ui.getData("subtitle") or {})[1] or {}
    local s=player_scale(width,height)
    local c=canvas(width,height,function(x) return (width-1440*s)/2+x*s end,
        function(y) return height-(900-y)*s end,function(v) return v*s end)
    if data.text and data.text~="" then
        local y=data.controlsVisible=="true" and 762 or 850
        c.rect(413,y,614,28,"bgVoid",0.55)
        c.text(data.text,413,y+19,614,15,"textPrimary","center")
    end
    return c.finish()
end
-- Toast：右上角通知。落位、尺寸与滑入偏移都由 C++ 侧同一套常量算出随数据下发，
-- 皮肤只决定外观（类型配色、色条、字号），换肤因此不会让动画曲线走样。
skin.surfaces.toast.render = function()
    local width,height=ui.getWindowSize()
    local rows=ui.getData("toasts") or {}
    local s=player_scale(width,height)
    local n=function(v) return tonumber(v) or 0 end
    local cfg=skin.surfaces.toast
    local c=canvas(width,height,function(x) return x end,function(y) return y end,
        function(v) return v end)
    local toastW=cfg.width*s
    local toastH=cfg.height*s
    local typeColor={ info="accentSecondary", success="stateSuccess",
                      warning="stateWarning", error="stateError" }
    local y=cfg.marginTop*s
    for _,t in ipairs(rows) do
        local a=math.max(0,math.min(1,n(t.alpha)))
        local role=typeColor[t.type] or "accentSecondary"
        local x=width-toastW-cfg.marginRight*s+n(t.slide)
        c.rect(x,y,toastW,toastH,"bgPanel",cfg.panelAlpha*a,false,8)
        c.rect(x,y,toastW,toastH,role,cfg.borderAlpha*a,true,8,1)
        -- 左侧类型色条：扫视时一眼可辨，不必读文字
        c.rect(x,y,cfg.accentBarWidth,toastH,role,a,false,8)
        c.text(t.icon or "",x+15,y+28,52,13,role)
        c.text(t.title or "",x+70,y+28,toastW-85,13,"textPrimary")
        local ty=y+48
        if (t.content or "")~="" then
            c.text(t.content,x+15,ty+8,toastW-30,12,"textPrimary"); ty=ty+18
        end
        if (t.detail or "")~="" then
            c.text(t.detail,x+15,ty+8,toastW-30,11,"textMuted")
        end
        y=y+toastH+cfg.spacing*s
    end
    return c.finish()
end

-- 开场 splash。数据来自 ui.getData("opening")：mediaPath / phase / elapsed / needsExtract。
-- elapsed 是宿主给的启动秒数，皮肤据此驱动 dots 呼吸动画，不必依赖 ui.getTime()
-- （splash 期间皮肤可能刚热加载，getTime 的基准与本次打开不对齐）。
skin.surfaces.opening.render = function()
    local width,height=ui.getWindowSize()
    local data=(ui.getData("opening") or {})[1] or {}
    local cfg=skin.surfaces.opening
    local n=function(v) return tonumber(v) or 0 end
    local t=n(data.elapsed)
    local c=canvas(width,height,function(v) return v end,function(v) return v end,
        function(v) return v end)

    c.rect(0,0,width,height,"bgVoid",cfg.overlayAlpha)

    local cardW=math.min(560,width*cfg.maxWidthRatio)
    local cardH=190
    local cx=(width-cardW)/2
    local cy=(height-cardH)/2
    c.rect(cx,cy,cardW,cardH,"bgPanel",0.96,false,6)
    c.rect(cx,cy,cardW,cardH,"accentPrimary",0.85,true,6,1.5)
    -- 四角短切线
    local L=cfg.cornerLength
    c.line(cx,cy,cx+L,cy,"accentPrimary",cfg.cornerThickness,1)
    c.line(cx,cy,cx,cy+L,"accentPrimary",cfg.cornerThickness,1)
    c.line(cx+cardW,cy,cx+cardW-L,cy,"accentPrimary",cfg.cornerThickness,1)
    c.line(cx+cardW,cy,cx+cardW,cy+L,"accentPrimary",cfg.cornerThickness,1)
    c.line(cx,cy+cardH,cx+L,cy+cardH,"accentPrimary",cfg.cornerThickness,1)
    c.line(cx,cy+cardH,cx,cy+cardH-L,"accentPrimary",cfg.cornerThickness,1)
    c.line(cx+cardW,cy+cardH,cx+cardW-L,cy+cardH,"accentPrimary",cfg.cornerThickness,1)
    c.line(cx+cardW,cy+cardH,cx+cardW,cy+cardH-L,"accentPrimary",cfg.cornerThickness,1)

    c.text("OPENING",cx,cy+cfg.titleOffsetY+cfg.titlePx,cardW,cfg.titlePx,"accentPrimary","center")
    c.text(data.phase or "",cx,cy+cfg.phaseOffsetY,cardW,12,"textPrimary","center")

    local shown=data.mediaPath or ""
    if #shown>64 then shown="..."..shown:sub(#shown-59) end
    c.text(shown,cx,cy+cfg.sourceOffsetY,cardW,12,"textMuted","center")

    local dy=cy+cardH-cfg.dotsBottomOffset
    for i=0,2 do
        local alpha=0.35+0.55*(0.5+0.5*math.sin(t*2-i*0.4))
        c.dot(cx+cardW*0.5+(i-1)*cfg.dotsGap,dy,cfg.dotRadius,"accentPrimary",alpha)
    end
    return c.finish()
end

-- 设置面板。按 main 分支 src/ui/Controller.cpp::renderSettingsModal() 的结构重建：
--
--   标题行（SETTINGS + X + 说明行 + 渐变细线）
--   ├─ 左侧固定导航：GENERAL / CAPTURE / LOGGING / APPEARANCE（四页，与 main 一致）
--   └─ 右侧：当前页的滚动内容；页内用 settingsSection 分小节，小节间一条分隔线
--   底部固定状态栏：三态文案（INVALID / WATCHING / APPLIED）+ 错误详情
--
-- 设置项全部来自 ui.getData("settings")；皮肤不硬编码字段名，C++ 注册表加一项就多一行。
-- 数据提供器的 group 是 playback/capture/logging/appearance，其中旧版把
-- PLAYBACK+SUBTITLES+NETWORK 都放在 GENERAL 页里，所以这里做一次页内归类。
skin.surfaces.settings.render = function()
    local width,height=ui.getWindowSize()
    local rows=ui.getData("settings") or {}
    local skinRows=ui.getData("skins") or {}
    local input=ui.getInput()
    local n=function(v) return tonumber(v) or 0 end
    local cfg=skin.surfaces.settings

    -- 面板几何：按设计稿比例缩放并居中（与 main 的 dialogW/H 计算一致）
    local panelW=math.min(cfg.maxWidth,width*cfg.widthRatio)
    local panelH=math.min(cfg.maxHeight,height*cfg.heightRatio)
    local px=(width-panelW)/2
    local py=(height-panelH)/2

    -- 四页导航（与 main 的 SettingsPage 枚举一一对应）
    -- 五页。旧版把播放/字幕/网络都塞进 GENERAL，内容高 772px 超出视口 444px，
    -- 代理被挤到滚动区里看不见；这里按主题拆开，每页都能一屏放下。
    local pages={ "general","playback","subtitle","proxy","capture","panels","logging","appearance" }
    local pageLabels={ general="GENERAL", playback="PLAYBACK", subtitle="SUBTITLE",
                       proxy="PROXY", capture="CAPTURE", panels="PANELS",
                       logging="LOGGING", appearance="APPEARANCE" }

    -- 内容区顶部的页头说明（版式图 y=263 那行）
    local pageSummary={
        general="Startup window geometry and control visibility.",
        playback="Playback behaviour, audio level and hardware decoding.",
        subtitle="Subtitle decoding, font scale and custom font path.",
        proxy="Network proxy used when opening streaming sources.",
        capture="Recording directory and screenshot options.",
        panels="Toggle the media info and statistics overlays.",
        logging="Diagnostics controls remain available while media keeps playing.",
        appearance="Change the shell without interrupting playback.",
    }

    local c=canvas(width,height,function(v) return v end,function(v) return v end,
        function(v) return v end)

    -- 遮罩：只暗化视频，不压在面板上
    c.rect(0,0,width,height,"bgVoid",cfg.overlayAlpha)

    -- 面板本体
    c.rect(px,py,panelW,panelH,"bgPanel",cfg.panelAlpha,false,6)
    c.rect(px,py,panelW,panelH,"accentPrimary",0.9,true,6,1)

    -- ── 标题行 ────────────────────────────────────────────────
    c.text("SETTINGS",px+cfg.paddingX,py+33,300,15,"accentPrimary")
    c.rail(px+cfg.paddingX,py+44,panelW-cfg.paddingX*2,"primaryRail",cfg.titleRailHeight)
    c.button("X","closeSettings",px+panelW-cfg.paddingX-cfg.closeButtonW,py+16,
             cfg.closeButtonW,22,"accentPrimary",nil,0.06,12)
    c.text("Live tweaks. Persisted to fluxplayer.ini on change.",
           px+cfg.paddingX,py+64,panelW-cfg.paddingX*2,11,"textMuted")
    c.line(px+cfg.paddingX,py+76,px+panelW-cfg.paddingX,py+76,"linePrimary",1,0.22)

    -- ── 左侧导航（固定，不随右侧滚动）──────────────────────────
    -- 版式图：面板顶 → nav 首项 = 100px
    local bodyTop=py+100
    -- 垂直预算：panelH = bodyTop偏移(100) + 列表区 + 状态栏预留(footerReserve)
    -- footerReserve 已含状态栏高与底部内边距，不能再额外扣边距——否则列表底部
    -- 会离状态栏留出一大段空白（之前多扣了 24，页面因此显得矮、容易误触发滚动）。
    local footerReserve=cfg.footerReserve
    local bodyH=math.max(120,panelH-(bodyTop-py)-footerReserve)
    local navX=px+cfg.paddingX
    local navW=math.min(cfg.navWidth,panelW*0.30)
    local contentX=navX+navW+cfg.navGap
    -- 内容区右侧预留滚动条宽度：控件宽度上限按扣除后的宽度算，
    -- 否则 wide 比例（0.75）的输入框会压到滚动条上。
    local contentW=panelW-cfg.paddingX*2-navW-cfg.navGap-cfg.scrollBarW

    local page=state.get("settingsPage") or "general"
    local known=false
    for _,name in ipairs(pages) do if name==page then known=true end end
    if not known then page="general" end

    for i,name in ipairs(pages) do
        local by=bodyTop+(i-1)*(cfg.navButtonH+cfg.navButtonGap)
        local active=(name==page)
        c.rect(navX,by,navW,cfg.navButtonH,
               active and "accentPrimary" or "linePrimary",active and 0.16 or 0.0,false,3)
        c.rect(navX,by,navW,cfg.navButtonH,
               active and "accentPrimary" or "lineSubtle",1,true,3,1)
        c.text(pageLabels[name],navX,by+cfg.navButtonH/2+4,navW,11,
               active and "accentPrimary" or "textSecondary","center")
        c.hit("setUiState",{key="settingsPage",value=name},navX,by,navW,cfg.navButtonH)
    end



    -- ── 右侧内容：按页归类 ──────────────────────────────────────
    -- C++ 的 group 是数据分组；GENERAL 页收 playback（含 SUBTITLES / NETWORK 两个小节）
    local inPage=function(row) return (row.group or "")==page end
    local visible={}
    for _,row in ipairs(rows) do if inPage(row) then visible[#visible+1]=row end end

    -- section 标题占位（对应 main 的 settingsSection：上留 sectionGap、下有细线）
    local sectionH=30
    local rowH={}
    local rowSection={}
    local contentH=0
    local lastSection=nil
    for i,row in ipairs(visible) do
        local sec=row.section or ""
        if sec~="" and sec~=lastSection then
            lastSection=sec
            rowSection[i]=true
            contentH=contentH+sectionH
        end
        local h=(row.type=="bool") and 34 or 54
        if (row.hint or "")~="" then h=h+20 end
        rowH[i]=h
        contentH=contentH+h+cfg.itemGapY
    end
    -- APPEARANCE 页额外占位：皮肤卡片 + 三栏按钮 + 下拉
    if page=="appearance" then contentH=contentH+120+cfg.activeCardH+60 end

    -- 内容区页头（版式图：页名 15px、说明 11px、下方一条细线）
    local headH=60
    c.text(pageLabels[page],contentX,bodyTop+22,contentW,15,"accentPrimary")
    c.text(pageSummary[page] or "",contentX,bodyTop+44,contentW,11,"textMuted")
    c.line(contentX,bodyTop+headH,contentX+contentW,bodyTop+headH,"linePrimary",1,0.22)

    -- 页头占 headH，剩余全部给列表（不再额外留 12px）
    local listY=bodyTop+headH
    local listH=bodyH-headH
    local scrollKey="settingsScroll_"..page
    local maxScroll=math.max(0,contentH-listH)
    local offset=math.min(n(state.get(scrollKey)),maxScroll)

    if input.wheel and input.wheel~=0 then
        local inside=(input.x>=contentX and input.x<=contentX+contentW and
                      input.y>=listY and input.y<=listY+listH)
        if inside then
            offset=math.max(0,math.min(maxScroll,offset-input.wheel*30))
            state.set(scrollKey,tostring(offset))
        end
    end

    -- ── APPEARANCE：当前皮肤卡片 + 皮肤下拉 + 三栏按钮（贴住内容顶部）──
    if page=="appearance" then
        local cardY=listY-offset
        local cardH=cfg.activeCardH
        if cardY+cardH>=listY and cardY<=listY+listH then
            -- 当前皮肤信息：取 skins 数据里的 current=true 那一行
            local cur=nil
            for _,r in ipairs(skinRows) do if r.current=="true" then cur=r end end
            c.rect(contentX,cardY,contentW,cardH,"bgPanelRaised",1,false,6)
            c.rect(contentX,cardY,contentW,cardH,"accentPrimary",0.7,true,6,1)
            if cur then
                c.text(cur.displayName or cur.id,contentX+cfg.activeCardPadding,
                       cardY+cfg.activeCardPadding+14,contentW*0.6,13,"textPrimary")
                c.text(string.format("v%s  /  %s  /  gen %s",
                       cur.version or "?",cur.source or "?",
                       tostring(cur.generation or "0")),
                       contentX+cfg.activeCardPadding,
                       cardY+cfg.activeCardPadding+cfg.activeCardTextGap+34,
                       contentW*0.6,11,"textMuted")
                -- 右侧 APPLIED chip
                local chipW,chipH=76,20
                local chipX=contentX+contentW-cfg.activeCardPadding-chipW
                local chipY=cardY+cfg.activeCardPadding
                c.rect(chipX,chipY,chipW,chipH,"accentPrimary",0.18,false,3)
                c.rect(chipX,chipY,chipW,chipH,"accentPrimary",1,true,3,1)
                c.text("APPLIED",chipX,chipY+14,chipW,10,"accentPrimary","center")
            end
        end
        -- 皮肤选择下拉：候选项来自 skins 数据，带来源与 INVALID 标记
        local comboY=cardY+cardH+cfg.activeCardAfterGap
        if comboY+30>=listY and comboY<=listY+listH then
            local opts={}
            local currentId=""
            for _,r in ipairs(skinRows) do
                if r.current=="true" then currentId=r.id end
            end
            for _,r in ipairs(skinRows) do
                if r.current~="true" then
                    opts[#opts+1]={value=r.id,label=r.label or r.id}
                end
            end
            c.add("combo",contentX,comboY,contentW,30,{
                id="skinId",action="configChanged",value=currentId,options=opts,
                background="bgField",border="textMuted",textColor="textPrimary",
                size=11,rounding=2})
        end
        -- 三栏动作按钮：RELOAD NOW / RESTORE DEFAULT / OPEN SKINS FOLDER
        local btnY=comboY+40
        if btnY+34>=listY and btnY<=listY+listH then
            local gap=8
            local btnW=(contentW-gap*2)/3
            c.button("RELOAD NOW","skinReload",contentX,btnY,btnW,32,"textPrimary")
            c.button("RESTORE DEFAULT","skinRestoreDefault",contentX+btnW+gap,btnY,btnW,32,"textPrimary")
            c.button("OPEN FOLDER","openSkinsFolder",contentX+(btnW+gap)*2,btnY,btnW,32,"textPrimary")
        end
    end

    -- ── 设置项行 ────────────────────────────────────────────────
    -- 版式对照 mockup_skin_settings.svg 与 cd4d28a 的 renderSettingsModal：
    --   * 标签与控件**同一行**（ImGui 的 "Label + widget" 默认语义）
    --   * 布尔项：复选框在左、文字紧跟其后
    --   * 控件宽度 = 内容宽 × ratio（对应 pageContentW * xxxFieldRatio）
    local y=(page=="appearance" and (listY-offset+cfg.activeCardH+120) or (listY-offset))
    for i,row in ipairs(visible) do
        if rowSection[i] then
            if y+sectionH>=listY and y<=listY+listH then
                -- settingsSection：上留 sectionGap、标题用 accentPrimary、下方留 sectionLabelGap
                c.text(row.section,contentX,y+cfg.sectionGap+11,contentW,11,"accentPrimary")
                c.line(contentX,y+cfg.sectionGap+17,contentX+contentW,y+cfg.sectionGap+17,
                       "linePrimary",1,0.22)
            end
            y=y+sectionH
        end
        local h=rowH[i]
        if y+h>=listY and y<=listY+listH then
            local label=row.label or row.key
            -- 标签列宽：mockup 里 "Log File Path" 到 x=566 起控件，约等于左列 28%
            local labelW=contentW*0.28
            local ctrlX=contentX+labelW
            -- 控件宽度：main 用 pageContentW * ratio（相对整个内容区），
            -- 这里同样按整个内容区算，但不允许超出标签列之后的剩余宽度。
            local wantW=contentW*(n(row.ratio)>0 and n(row.ratio) or 0.75)
            local ctrlW=math.min(wantW,contentW-labelW)

            if row.type=="bool" then
                -- 复选框在左、标签在右，同行居中（ImGui::Checkbox("Text", &v) 的语义）
                c.add("checkbox",contentX,y+7,24,22,{
                    id=row.key,action="configChanged",text="",value=(row.value=="true")})
                c.text(label,contentX+32,y+18,contentW-32,12,"textPrimary")
                if (row.hint or "")~="" then
                    c.text(row.hint,contentX+32,y+40,contentW-32,10,"textMuted")
                end
            else
                -- 标签与控件同一行，垂直居中对齐
                c.text(label,contentX,y+22,labelW-12,12,"textPrimary")
                local ctrlY=y+8

                if row.type=="option" then
                    c.add("combo",ctrlX,ctrlY,ctrlW,28,{
                        id=row.key,action="configChanged",value=row.value,
                        options=optionsFromRow(row),
                        background="bgField",border="textMuted",textColor="textPrimary",
                        size=11,rounding=2})
                    local opts=optionsFromRow(row)
                    local matched=false
                    for _,o in ipairs(opts) do if o.value==row.value then matched=true end end
                    if not matched and row.value~="" then
                        c.text("("..row.value..")",ctrlX+ctrlW+10,ctrlY+20,140,11,"stateWarning")
                    end
                elseif row.type=="int" then
                    -- 纯文本框，无 -/+ 步进按钮（后端按失焦/回车提交）。step 已不再被后端读取。
                    c.add("numberInput",ctrlX,ctrlY,ctrlW,28,{
                        id=row.key,action="configChanged",value=n(row.value),
                        min=n(row.min),max=n(row.max),
                        background="bgField",border="textMuted",textColor="textPrimary",
                        size=11,rounding=2})
                elseif row.type=="number" then
                    -- main 用 SliderFloat(..., "%.2f")，数值贴在滑块右侧
                    local sliderW=ctrlW
                    c.slider(row.key,n(row.value),n(row.min),n(row.max),
                             ctrlX,ctrlY+4,sliderW,20,"accentPrimary",true,"configChanged")
                    c.text(string.format("%.2f",n(row.value)),
                           ctrlX+sliderW+10,ctrlY+20,70,11,"textSecondary")
                else
                    -- 路径类设置：控件右侧跟一个 Browse 按钮。是否显示、弹文件还是目录选择器
                    -- 都由 C++ 下发的 row.pathKind 决定（none/file/dir），皮肤不硬编码 key。
                    -- 先把输入框收窄，保证「输入框 + 间距 + 按钮」恰好落在内容区内，
                    -- 不会压到右侧滚动条上。
                    local kind=row.pathKind or "none"
                    local hasBrowse=(kind=="file" or kind=="dir")
                    local btnW,btnGap=76,8
                    if hasBrowse then
                        ctrlW=math.max(120,ctrlW-btnW-btnGap)
                    end
                    c.add("input",ctrlX,ctrlY,ctrlW,28,{
                        id=row.key,value=row.value,stateKey=row.key,action="configChanged",
                        background="bgField",border="textMuted",textColor="textPrimary",
                        size=11,rounding=2})
                    if hasBrowse then
                        c.button("Browse","browsePath",ctrlX+ctrlW+btnGap,ctrlY,btnW,28,
                                 "textPrimary",{key=row.key},0.06,11)
                    end
                end

                if (row.hint or "")~="" then
                    c.text(row.hint,contentX,y+38,contentW,10,"textMuted")
                end
            end
        end
        y=y+h+cfg.itemGapY
    end

    -- 滚动条：内容超出视口时给出位置反馈
    if maxScroll>0 then
        -- 滚动条落在预留带内（内容区已扣掉 scrollBarW），故不会压住控件
        local trackX=px+panelW-cfg.paddingX-cfg.scrollBarW+4
        c.rect(trackX,listY,3,listH,"linePrimary",0.22,false,2)
        local thumbH=math.max(24,listH*listH/contentH)
        local thumbY=listY+(listH-thumbH)*(offset/maxScroll)
        c.rect(trackX,thumbY,3,thumbH,"accentPrimary",0.75,false,2)
    end

    -- ── 底部状态栏 ────────────────────────────────────────────
    -- 三态与 main 一致：皮肤加载失败 / 热加载中 / 已应用
    local curStatus=nil
    for _,r in ipairs(skinRows) do if r.current=="true" then curStatus=r end end
    local skinError=curStatus and (curStatus.error or "") or ""
    local hotReload=curStatus and curStatus.hotReload=="true"
    local isErr=(skinError~="")
    local statusText
    if not curStatus then
        statusText="NO SKIN STATUS"          -- 数据提供器未就绪时不要崩，给出显式占位
    elseif isErr then
        statusText="INVALID - USING PREVIOUS SKIN"
    elseif hotReload then
        statusText=string.format("WATCHING / %s / GEN %s",
                                 curStatus.id or "",tostring(curStatus.generation or "0"))
    else
        statusText=string.format("APPLIED / %s / GEN %s",
                                 curStatus.id or "",tostring(curStatus.generation or "0"))
    end

    local footY=py+panelH-cfg.paddingY-cfg.statusBarH
    local barX=px+cfg.paddingX
    local barW=panelW-cfg.paddingX*2
    c.rect(barX,footY,barW,cfg.statusBarH,isErr and "stateError" or "accentPrimary",
           isErr and 0.15 or 0.10,false,3)
    c.rect(barX,footY,barW,cfg.statusBarH,isErr and "stateError" or "accentPrimary",
           isErr and 0.55 or 0.55,true,3,1)
    c.dot(barX+cfg.statusDotInsetX,footY+cfg.statusBarH/2,cfg.statusDotRadius,
          isErr and "stateError" or "accentPrimary")
    c.text(statusText,barX+cfg.statusTextInsetX,footY+21,barW-cfg.statusTextInsetX-10,11,
           isErr and "stateError" or "accentPrimary")
    -- 错误详情（main 在状态栏下方追加一行 lastError）
    if isErr then
        c.text(skinError,barX,footY+cfg.statusBarH+16,barW,10,"textMuted")
    end

    return c.finish()
end

skin.surfaces.home.render = render_home
skin.surfaces.player = skin.surfaces.player or {}
skin.surfaces.player.render = render_player
skin.surfaces.merge = skin.surfaces.merge or {}
skin.surfaces.merge.render = render_merge
-- toast / opening / settings 的 render 都已在上面就地赋值，这里不再重复登记。
return skin
