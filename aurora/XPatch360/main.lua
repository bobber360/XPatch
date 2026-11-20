-- XPatch360 - patch toggler for Aurora (Utilities menu)
-- Install: copy this folder to  Aurora\User\Scripts\Utility\XPatch360\
-- Lists the Xenia patch files in Hdd:\Patches\ and flips their
-- "is_enabled = true/false" lines. The XPatch360 plugin applies the enabled
-- patches when the game starts.

scriptTitle       = "XPatch360"
scriptAuthor      = "bobber360 & Claude"
scriptVersion     = 1.2
scriptDescription = "Xenia patches for RGH"
scriptPermissions = { "filesystem", "content" }
-- PNG may or may not be accepted by your Aurora version; the official samples
-- use a compiled .xur (scriptIcon = "icon\\icon.xur"). If no icon shows, see README.
scriptIcon        = "icon.png"

-- Hdd:\Patches\ in the path forms Aurora might use; the first one that
-- lists files wins
local PATCH_DIRS = {
    "Hdd1:\\Patches\\",
    "\\Xbox360\\System\\Hdd1\\Patches\\",
    "Hdd:\\Patches\\",
    "\\Device\\Harddisk0\\Partition1\\Patches\\",
}
local PATTERNS = { "", "*", "*.*", "*.toml" }

local NotificationType = { Information = 0, System = 1, Warning = 2, Error = 3 }

-- ---------------------------------------------------------------------------
-- helpers
-- ---------------------------------------------------------------------------
local function entryName(e)
    if type(e) == "string" then return e end
    if type(e) == "table" then return e.Name or e.name or e.FileName end
    return nil
end

local function baseName(path)
    return string.match(path, "([^\\]+)$") or path
end

-- returns list of full paths, plus a report of what was tried
local function findPatchFiles()
    local report = {}
    for _, d in ipairs(PATCH_DIRS) do
        for _, pat in ipairs(PATTERNS) do
            local ok, files = pcall(FileSystem.GetFiles, d .. pat)
            local count, tomls = 0, {}
            if ok and type(files) == "table" then
                for _, e in pairs(files) do
                    count = count + 1
                    local n = entryName(e)
                    if n ~= nil and string.match(string.lower(n), "%.toml$") then
                        table.insert(tomls, d .. baseName(n))
                    end
                end
            end
            if #tomls > 0 then return tomls, report end
            local what = (not ok) and "error" or (files == nil and "nil" or (count .. " entries"))
            if ok and type(files) == "table" and count > 0 then
                -- show what an entry looks like, so the field name can be fixed
                local _, first = next(files)
                if type(first) == "table" then
                    local keys = {}
                    for k, v in pairs(first) do table.insert(keys, tostring(k) .. "=" .. tostring(v)) end
                    what = what .. " {" .. table.concat(keys, ", ") .. "}"
                else
                    what = what .. " [" .. tostring(first) .. "]"
                end
            end
            table.insert(report, d .. pat .. "  -> " .. what)
        end
    end
    return nil, report
end

-- split into lines, keeping each line's own ending ("\r\n", "\n" or "")
local function splitLines(data)
    local lines, pos = {}, 1
    while pos <= #data do
        local s, e = string.find(data, "\r?\n", pos)
        if s == nil then
            table.insert(lines, { text = string.sub(data, pos), eol = "" })
            break
        end
        table.insert(lines, { text = string.sub(data, pos, s - 1), eol = string.sub(data, s, e) })
        pos = e + 1
    end
    return lines
end

-- TOML basic string "..." (with \" \\ \n \t escapes) or literal '...';
-- anything after the closing quote (a # comment) is ignored
local function unquote(v)
    local first = string.sub(v, 1, 1)
    if first == "'" then
        return string.match(v, "^'(.-)'") or v
    end
    if first ~= '"' then return v end
    local out, i = {}, 2
    while i <= #v do
        local c = string.sub(v, i, i)
        if c == '\\' then
            local n = string.sub(v, i + 1, i + 1)
            if n == "n" then table.insert(out, "\n")
            elseif n == "t" then table.insert(out, " ")
            elseif n ~= "" then table.insert(out, n) end
            i = i + 2
        elseif c == '"' then
            break
        else
            table.insert(out, c)
            i = i + 1
        end
    end
    return table.concat(out)
end

-- parse what we need: title and the [[patch]] blocks
local function loadToml(path)
    local data = FileSystem.ReadFile(path)
    if data == nil then return nil end
    local doc = { path = path, lines = splitLines(data), patches = {}, title = nil }
    local cur = nil
    local inSub = false
    local curWrite = nil
    for i, l in ipairs(doc.lines) do
        local t = string.match(l.text, "^%s*(.-)%s*$")
        if string.match(t, "^%[%[patch%]%]") then
            cur = { header = i, enabledLine = nil, on = false, name = "Patch " .. (#doc.patches + 1),
                    writes = {} }
            table.insert(doc.patches, cur)
            inSub = false
            curWrite = nil
        elseif string.sub(t, 1, 1) == "[" then
            inSub = true                              -- [[patch.be32]] etc.
            curWrite = nil
            local wtype = string.match(t, "^%[%[patch%.([%w_]+)%]%]")
            if cur ~= nil and wtype ~= nil then
                curWrite = { type = string.lower(wtype) }
                table.insert(cur.writes, curWrite)
            end
        else
            local key, val = string.match(t, "^([%w_]+)%s*=%s*(.-)%s*$")
            if key ~= nil and curWrite ~= nil and inSub then
                if key == "address" then curWrite.addr = tonumber(string.match(val, "^0[xX](%x+)") or "", 16)
                elseif key == "value" then curWrite.value = val end
            end
            if key ~= nil then
                if cur == nil then
                    if key == "title_name" then doc.title = unquote(val)
                    elseif key == "title_id" then doc.titleId = string.upper(unquote(val)) end
                elseif not inSub then
                    if key == "name" then cur.name = unquote(val)
                    elseif key == "author" then cur.author = unquote(val)
                    elseif key == "desc" then cur.desc = unquote(val)
                    elseif key == "is_enabled" then
                        cur.enabledLine = i
                        cur.on = (string.sub(val, 1, 4) == "true")
                    end
                end
            end
        end
    end
    return doc
end

local function saveToml(doc)
    local out = {}
    for _, l in ipairs(doc.lines) do
        table.insert(out, l.text)
        table.insert(out, l.eol)
    end
    return FileSystem.WriteFile(doc.path, table.concat(out))
end

-- flip one patch; rewrites its is_enabled line (or adds one after [[patch]])
local function togglePatch(doc, idx)
    local p = doc.patches[idx]
    local newState = not p.on
    local value = newState and "true" or "false"
    if p.enabledLine ~= nil then
        local l = doc.lines[p.enabledLine]
        l.text = string.gsub(l.text, "(is_enabled%s*=%s*)%a+", "%1" .. value, 1)
    else
        local hdr = doc.lines[p.header]
        local indent = string.match(hdr.text, "^(%s*)") or ""
        local eol = (hdr.eol ~= "" and hdr.eol) or "\r\n"
        if hdr.eol == "" then hdr.eol = eol end
        table.insert(doc.lines, p.header + 1, { text = indent .. "    is_enabled = " .. value, eol = eol })
    end
    if not saveToml(doc) then return false end
    return true
end

-- returns 1-based index of the chosen item, or nil if cancelled
local function popup(title, emptyText, items)
    local ret = Script.ShowPopupList(title, emptyText, items)
    if ret == nil or ret.Canceled then return nil end
    local sel = ret.Selected
    if sel == nil then return nil end
    if type(sel) == "table" then
        if sel.Key ~= nil then return tonumber(sel.Key) end
        if sel.Value ~= nil then
            for i, v in ipairs(items) do if v == sel.Value then return i end end
        end
        return nil
    end
    return tonumber(sel)
end

-- Many Xenia descriptions only point to the Xenia README ("See note about
-- framerate patches in the README."), which is about emulator settings.
-- Drop those sentences and, for framerate notes, say what it means on a console.
local FPS_NOTE = "Framerate patch. On a real Xbox 360 the picture is limited to 60 Hz, " ..
                 "so anything above 60 FPS won't be visible. If the game can't hold the " ..
                 "higher rate, frame pacing can feel uneven."

local function consoleDesc(desc)
    if desc == nil or desc == "" then return "No description." end
    if not string.find(string.lower(desc), "readme", 1, true) then return desc end

    local kept, fpsNote = {}, false
    -- split into sentences (keep the end mark and line breaks with the sentence);
    -- a last sentence without an end mark gets a line break so it is split too
    local src = desc
    if not string.find(src, "[%.!?\n]%s*$") then src = src .. "\n" end
    for s in string.gmatch(src, "(.-[%.!?\n]+)%s*") do
        local low = string.lower(s)
        if string.find(low, "readme", 1, true) then
            if string.find(low, "framerate", 1, true) or string.find(low, "fps", 1, true)
               or string.find(low, "frame rate", 1, true) then
                fpsNote = true
            end
        else
            table.insert(kept, s)
        end
    end
    local text = ""
    for _, s in ipairs(kept) do
        if text == "" or string.find(text, "\n$") then text = text .. s
        else text = text .. " " .. s end
    end
    text = string.gsub(text, "^%s+", "")
    text = string.gsub(text, "%s+$", "")
    if fpsNote then
        text = (text ~= "") and (text .. "\n\n" .. FPS_NOTE) or FPS_NOTE
    end
    if text == "" then return "No description." end
    return text
end


-- ---------------------------------------------------------------------------
-- settings (XPatch360.ini, read by the plugin every time a game starts)
-- ---------------------------------------------------------------------------
local patchDir = nil          -- folder of the patch files, set in main()

-- plain string (no library call at load time: Aurora reads the script header
-- before the libraries are there)
local INI_DEFAULT =
    "; XPatch360 settings - read at boot and every time a game starts\r\n" ..
    "; 1 = on, 0 = off. Can also be changed in Aurora: Utilities -> XPatch360 -> Settings\r\n" ..
    "[XPatch360]\r\n" ..
    "notify      = 1     ; notification when a game starts\r\n" ..
    "log         = 1     ; 1 = full log, 0 = errors only\r\n" ..
    "log_max_kb  = 64    ; a bigger log is deleted at boot and started again\r\n" ..
    "early_patch = 0     ; patch before the game starts - for patches marked (early patching required)\r\n" ..
    "debug       = 0     ; debug mode: memory dumps (XPatch360.dump), debug menu in Aurora\r\n"

-- XPatch360's own files live in Hdd:\Patches\XPatch360\ (the plugin creates it)
local function dataDir()  return patchDir .. "XPatch360\\" end
local function iniPath()  return dataDir() .. "XPatch360.ini" end
local function dumpPath() return dataDir() .. "XPatch360.dump" end

local function readIni()
    local data = FileSystem.ReadFile(iniPath())
    if data == nil or data == "" then data = INI_DEFAULT end
    local values = {}
    for _, l in ipairs(splitLines(data)) do
        local k, v = string.match(l.text, "^%s*([%w_]+)%s*=%s*([^;%s]*)")
        if k ~= nil then values[string.lower(k)] = v end
    end
    return values, data
end

local function iniOn(v) return v == "1" or v == "true" or v == "on" end

-- change one value, keeping the spacing and the comment of its line
local function setIni(key, value)
    local _, data = readIni()
    local lines, found = splitLines(data), false
    for _, l in ipairs(lines) do
        local pre, old, post = string.match(l.text, "^(%s*" .. key .. "%s*=%s*)([^;%s]*)(.*)$")
        if pre ~= nil then
            -- keep the comment column: pad/cut the spaces after the value
            local pad = string.match(post, "^(%s*)") or ""
            local rest = string.sub(post, #pad + 1)
            local width = #old + #pad
            local spaces = width - #value
            if spaces < 1 then spaces = (rest ~= "") and 1 or 0 end
            l.text = pre .. value .. string.rep(" ", spaces) .. rest
            found = true
        end
    end
    if not found then
        table.insert(lines, { text = key .. " = " .. value, eol = "\r\n" })
    end
    local out = {}
    for _, l in ipairs(lines) do table.insert(out, l.text); table.insert(out, l.eol) end
    return FileSystem.WriteFile(iniPath(), table.concat(out))
end

-- one line in XPatch360.dump (not twice); the plugin removes it after the dump
local function addDumpLine(line)
    local data = FileSystem.ReadFile(dumpPath()) or ""
    for _, l in ipairs(splitLines(data)) do
        if string.upper(string.match(l.text, "^%s*(.-)%s*$")) == string.upper(line) then return true end
    end
    if data ~= "" and not string.find(data, "\n$") then data = data .. "\r\n" end
    return FileSystem.WriteFile(dumpPath(), data .. line .. "\r\n")
end

local LOG_SIZES = { 32, 64, 128, 256, 512 }

local function settingsMenu()
    while true do
        local v = readIni()
        local debug = iniOn(v.debug)
        local logKb = tonumber(v.log_max_kb) or 64
        local items = {
            "Notifications: " .. (iniOn(v.notify) and "ON" or "OFF"),
            "Log: " .. (iniOn(v.log) and "full" or "errors only"),
            "Log size limit: " .. logKb .. " KB",
            "Early patching: " .. (iniOn(v.early_patch) and "ON" or "OFF"),
            "Debug mode: " .. (debug and "ON" or "OFF"),
        }
        if debug then table.insert(items, "[ Debug: dump the next game I start ]") end

        local choice = popup(scriptTitle .. " - Settings (from the next game start)", "", items)
        if choice == nil then return end
        local ok = true
        if choice == 1 then ok = setIni("notify", iniOn(v.notify) and "0" or "1")
        elseif choice == 2 then ok = setIni("log", iniOn(v.log) and "0" or "1")
        elseif choice == 3 then
            local nextKb = LOG_SIZES[1]
            for i, n in ipairs(LOG_SIZES) do
                if n == logKb then nextKb = LOG_SIZES[i % #LOG_SIZES + 1] end
            end
            ok = setIni("log_max_kb", tostring(nextKb))
        elseif choice == 4 then ok = setIni("early_patch", iniOn(v.early_patch) and "0" or "1")
        elseif choice == 5 then ok = setIni("debug", debug and "0" or "1")
        elseif choice == 6 and debug then
            ok = addDumpLine("next code")
            if ok then
                Script.ShowNotification("The next game you start will be dumped", NotificationType.Information)
            end
        end
        if not ok then Script.ShowNotification("Could not save the settings", NotificationType.Error) end
    end
end

local function debugOn()
    local v = readIni()
    return iniOn(v.debug)
end


-- ---------------------------------------------------------------------------
-- conflict check: two patches writing the same bytes (only the later one works)
-- ---------------------------------------------------------------------------
local TYPE_SIZE = { be8 = 1, be16 = 2, be32 = 4, be64 = 8, f32 = 4, f64 = 8 }

local function writeSize(w)
    local n = TYPE_SIZE[w.type]
    if n then return n end
    local v = w.value or ""
    if w.type == "string" then return math.max(#unquote(v), 1) end
    if w.type == "u16string" then return math.max(#unquote(v) * 2, 2) end
    if w.type == "array" then
        local hex = string.gsub(string.gsub(unquote(v), "0[xX]", ""), "[^%x]", "")
        return math.max(math.floor(#hex / 2), 1)
    end
    return 4
end

local function overlaps(a, b)
    for _, wa in ipairs(a.writes or {}) do
        if wa.addr then
            local sa = writeSize(wa)
            for _, wb in ipairs(b.writes or {}) do
                if wb.addr and wa.addr < wb.addr + writeSize(wb) and wb.addr < wa.addr + sa then
                    return true
                end
            end
        end
    end
    return false
end

-- names of the other patches in this file that are ON and write the same bytes
local function conflictsOf(doc, idx)
    local list = {}
    for i, q in ipairs(doc.patches) do
        if i ~= idx and q.on and overlaps(doc.patches[idx], q) then table.insert(list, q.name) end
    end
    return list
end

-- ---------------------------------------------------------------------------
-- screens
-- ---------------------------------------------------------------------------
-- every patch of the file off, saved once
local function allOff(doc)
    local changed = 0
    for _, p in ipairs(doc.patches) do
        if p.on and p.enabledLine ~= nil then
            local l = doc.lines[p.enabledLine]
            l.text = string.gsub(l.text, "(is_enabled%s*=%s*)%a+", "%1false", 1)
            changed = changed + 1
        end
    end
    if changed == 0 then return true, 0 end
    return saveToml(doc), changed
end

-- "(console)" and "(console, early patching required)" both count
local function isConsolePatch(p)
    return string.find(string.lower(p.name), "(console", 1, true) ~= nil
end

local function editGame(path, fileLabel)
    while true do
        local doc = loadToml(path)              -- reload: line numbers change on insert
        if doc == nil or #doc.patches == 0 then
            Script.ShowNotification("No patches in " .. baseName(path), NotificationType.Warning)
            return
        end

        -- entries: special actions first, then the patches; "(console)" patches
        -- (made for a real Xbox 360) before the Xenia ones
        local items, actions = {}, {}
        local function add(text, action) table.insert(items, text); table.insert(actions, action) end

        add("[ Turn all patches off ]", { kind = "alloff" })
        local titleId = doc.titleId or string.upper(string.match(baseName(path), "^(%x%x%x%x%x%x%x%x)") or "")
        if titleId ~= "" and debugOn() then
            add("[ Debug: dump this game's code on next start ]", { kind = "dump" })
        end
        for pass = 1, 2 do
            for i, p in ipairs(doc.patches) do
                if (pass == 1) == isConsolePatch(p) then
                    local mark = (p.on and #conflictsOf(doc, i) > 0) and "  (!) conflict" or ""
                    add((p.on and "[ON]  " or "[OFF] ") .. p.name .. mark, { kind = "patch", index = i })
                end
            end
        end

        local header = doc.title or baseName(path)
        if fileLabel then header = header .. " - " .. fileLabel end
        local choice = popup(header, "No patches", items)
        if choice == nil or actions[choice] == nil then return end
        local act = actions[choice]

        if act.kind == "alloff" then
            local ok, n = allOff(doc)
            if not ok then
                Script.ShowNotification("Could not save " .. baseName(path), NotificationType.Error)
            elseif n == 0 then
                Script.ShowNotification("All patches are already off", NotificationType.Information)
            else
                Script.ShowNotification(n .. " patch" .. (n == 1 and "" or "es") .. " turned off",
                                        NotificationType.Information)
            end

        elseif act.kind == "dump" then
            if addDumpLine(titleId .. " code") then
                Script.ShowNotification("Code dump at the next start of this game", NotificationType.Information)
            else
                Script.ShowNotification("Could not write XPatch360.dump", NotificationType.Error)
            end

        else
            local idx = act.index
            local p = doc.patches[idx]

            -- info window: author and description, then turn on/off or go back
            local info = "Author: " .. ((p.author and p.author ~= "") and p.author or "unknown") ..
                         "\n\n" .. consoleDesc(p.desc)
            local clash = conflictsOf(doc, idx)
            if #clash > 0 then
                info = "WARNING: writes the same address as " .. table.concat(clash, ", ") ..
                       " (ON). Only one of them can work - turn the other one off.\n\n" .. info
            end
            if #info > 700 then info = string.sub(info, 1, 697) .. "..." end
            local action = p.on and "Turn OFF" or "Turn ON"
            local ok, ret = pcall(Script.ShowMessageBox, p.name, info, action, "Back")

            if ok and ret ~= nil and not ret.Canceled and ret.Button == 1 then
                if togglePatch(doc, idx) then
                    Script.ShowNotification(p.name .. (p.on and ": OFF" or ": ON"),
                                            NotificationType.Information)
                else
                    Script.ShowNotification("Could not save " .. baseName(path), NotificationType.Error)
                end
            end
        end
    end
end

function main()
    local files, report = findPatchFiles()
    if files == nil then
        local msg = "No .toml files found in Hdd:\\Patches\\\n\nTried:\n" .. table.concat(report, "\n")
        -- also save the report, so it can be read on the PC
        pcall(FileSystem.WriteFile, Script.GetBasePath() .. "xpatch_debug.txt", msg)
        Script.ShowMessageBox(scriptTitle, msg, "OK")
        return
    end

    patchDir = string.match(files[1], "^(.*\\)") or "Hdd1:\\Patches\\"

    -- Group the files by title ID, using only the file names
    -- ("545408A7 - Grand Theft Auto V (TU27).toml"), so hundreds of files
    -- don't have to be read just to show the list.
    local groups, byId = {}, {}
    for _, path in ipairs(files) do
        local n = baseName(path)
        n = string.gsub(n, "%.patch%.toml$", "")
        n = string.gsub(n, "%.toml$", "")
        local id, rest = string.match(n, "^(%x%x%x%x%x%x%x%x)%s*%-?%s*(.*)$")
        if id ~= nil then
            id = string.upper(id)
            -- "(TU27)", "(Japan)", "(v1.0)" -> tags; the rest is the game name
            local tags, tu = {}, nil
            for tag in string.gmatch(rest, "%(([^)]*)%)") do
                local num = string.match(string.lower(tag), "^tu(%d+)$")
                if num then tu = tonumber(num) else table.insert(tags, tag) end
            end
            local name = string.gsub(string.gsub(rest, "%b()", ""), "^%s*(.-)%s*$", "%1")
            local label = tu and ("TU" .. tu) or "Base game (no TU)"
            if #tags > 0 then label = label .. "  (" .. table.concat(tags, ", ") .. ")" end

            local g = byId[id]
            if g == nil then
                g = { id = id, name = "", files = {} }
                byId[id] = g
                table.insert(groups, g)
            end
            -- prefer the shortest non-empty name among the files
            if name ~= "" and (g.name == "" or #name < #g.name) then g.name = name end
            table.insert(g.files, { path = path, label = label, tu = tu or 0, extra = #tags })
        end
    end
    for _, g in ipairs(groups) do
        if g.name == "" then g.name = g.id end
        g.sortKey = string.lower(g.name)
        -- base game first, then by TU number; plain files before tagged ones
        table.sort(g.files, function(a, b)
            if a.tu ~= b.tu then return a.tu < b.tu end
            if a.extra ~= b.extra then return a.extra < b.extra end
            return a.label < b.label
        end)
    end
    table.sort(groups, function(a, b) return a.sortKey < b.sortKey end)

    -- which of these games are in Aurora's library (one lookup per title ID)
    local lookupWorks = false
    local canLookup = (type(Content) == "table" and type(Content.FindContent) == "function")
    local installed = {}
    for _, g in ipairs(groups) do
        if canLookup then
            local ok, found = pcall(Content.FindContent, tonumber(g.id, 16))
            if ok and type(found) == "table" then
                lookupWorks = true
                if next(found) ~= nil then table.insert(installed, g) end
            end
        end
    end

    -- The file names are cut to 42 characters on the console, so take the
    -- game's real name from title_name inside its first file (read on demand).
    local function resolveNames(list)
        local changed = false
        for _, g in ipairs(list) do
            if not g.nameRead then
                g.nameRead = true
                local data = FileSystem.ReadFile(g.files[1].path)
                local t = data and string.match(data, 'title_name%s*=%s*"([^"\r\n]*)"')
                if t and t ~= "" then
                    g.name = t
                    g.sortKey = string.lower(t)
                    changed = true
                end
            end
        end
        if changed then table.sort(list, function(a, b) return a.sortKey < b.sortKey end) end
    end

    -- one game: its files, or straight to the patches if it has only one file
    local function openGame(g)
        if #g.files == 1 then
            editGame(g.files[1].path, g.files[1].label)
            return
        end
        while true do
            local items = {}
            for _, f in ipairs(g.files) do table.insert(items, f.label) end
            local choice = popup(g.name .. " - patch files", "No patch files", items)
            if choice == nil or g.files[choice] == nil then return end
            editGame(g.files[choice].path, g.files[choice].label)
        end
    end

    -- installed games only; if the lookup isn't available, show everything
    local showAll = not lookupWorks
    while true do
        local list = showAll and groups or installed
        if showAll and #groups > 50 then
            pcall(Script.SetStatus, "Reading game names...")
        end
        resolveNames(list)
        local items = { "[ Settings ]" }
        for _, g in ipairs(list) do
            local count = #g.files
            table.insert(items, g.name .. (count > 1 and ("  (" .. count .. " files)") or ""))
        end
        if not showAll then table.insert(items, "[ Show all games (" .. #groups .. ") ]") end

        local title = scriptTitle .. " v1.2" .. (showAll and " - all games" or " - installed games")
        local choice = popup(title, "No patch files for your installed games", items)
        if choice == nil then
            if showAll and lookupWorks then showAll = false else return end
        elseif choice == 1 then
            settingsMenu()
        elseif not showAll and choice == #items then
            showAll = true
        elseif list[choice - 1] ~= nil then
            openGame(list[choice - 1])
        end
    end
end
