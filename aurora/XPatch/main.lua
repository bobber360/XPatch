-- XPatch - patch toggler for Aurora (Utilities menu)
-- Install: copy this folder to  Aurora\User\Scripts\Utility\XPatch\
-- Lists the Xenia patch files in Hdd:\Patches\ and flips their
-- "is_enabled = true/false" lines. The XPatch plugin applies the enabled
-- patches when the game starts.

scriptTitle       = "XPatch"
scriptAuthor      = "bobber360 & Claude"
scriptVersion     = 1
scriptDescription = "Turn Xenia game patches on/off"
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

local function unquote(v)
    local q = string.match(v, '^"(.-)"')
    return q or v
end

-- parse what we need: title and the [[patch]] blocks
local function loadToml(path)
    local data = FileSystem.ReadFile(path)
    if data == nil then return nil end
    local doc = { path = path, lines = splitLines(data), patches = {}, title = nil }
    local cur = nil
    local inSub = false
    for i, l in ipairs(doc.lines) do
        local t = string.match(l.text, "^%s*(.-)%s*$")
        if string.match(t, "^%[%[patch%]%]") then
            cur = { header = i, enabledLine = nil, on = false, name = "Patch " .. (#doc.patches + 1) }
            table.insert(doc.patches, cur)
            inSub = false
        elseif string.sub(t, 1, 1) == "[" then
            inSub = true                              -- [[patch.be32]] etc.
        else
            local key, val = string.match(t, "^([%w_]+)%s*=%s*(.-)%s*$")
            if key ~= nil then
                if cur == nil then
                    if key == "title_name" then doc.title = unquote(val) end
                elseif not inSub then
                    if key == "name" then cur.name = unquote(val)
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

-- ---------------------------------------------------------------------------
-- screens
-- ---------------------------------------------------------------------------
local function editGame(path, fileLabel)
    while true do
        local doc = loadToml(path)              -- reload: line numbers change on insert
        if doc == nil or #doc.patches == 0 then
            Script.ShowNotification("No patches in " .. baseName(path), NotificationType.Warning)
            return
        end

        local items = {}
        for _, p in ipairs(doc.patches) do
            table.insert(items, (p.on and "[ON]  " or "[OFF] ") .. p.name)
        end

        local header = doc.title or baseName(path)
        if fileLabel then header = header .. " - " .. fileLabel end
        local choice = popup(header, "No patches", items)
        if choice == nil or doc.patches[choice] == nil then return end

        local p = doc.patches[choice]
        if togglePatch(doc, choice) then
            Script.ShowNotification(p.name .. (p.on and ": OFF" or ": ON"),
                                    NotificationType.Information)
        else
            Script.ShowNotification("Could not save " .. baseName(path), NotificationType.Error)
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
        local items = {}
        for _, g in ipairs(list) do
            local count = #g.files
            table.insert(items, g.name .. (count > 1 and ("  (" .. count .. " files)") or ""))
        end
        if not showAll then table.insert(items, "[ Show all games (" .. #groups .. ") ]") end

        local title = scriptTitle .. (showAll and " - all games" or " - installed games")
        local choice = popup(title, "No patch files for your installed games", items)
        if choice == nil then
            if showAll and lookupWorks then showAll = false else return end
        elseif not showAll and choice == #items then
            showAll = true
        elseif list[choice] ~= nil then
            openGame(list[choice])
        end
    end
end
