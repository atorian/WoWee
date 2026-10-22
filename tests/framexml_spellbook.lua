-- Real FrameXML and native bindings, with the runner's learned-spell fixture.
local case = woweeSpellbookCase
if case == 'panels' then
    for _, frame in ipairs({SpellBookFrame, CharacterFrame, QuestLogFrame, ContainerFrame1, PVPParentFrame, BattlefieldFrame, WorldStateScoreFrame}) do
        assert(not frame:GetScript('OnDragStart'), frame:GetName() .. ' invents a body drag')
    end
elseif case == 'selection' then
    for i = 1, SPELLS_PER_PAGE do
        local button = _G['SpellButton' .. i]
        assert(not button:GetChecked(), button:GetName() .. ' is selected without a selected spell')
    end
    local button = SpellButton1
    for _, value in ipairs({false, 0, '0', 'false'}) do
        button:SetChecked(true)
        button:SetChecked(value)
        assert(not button:GetChecked(), 'failed to clear selection: ' .. tostring(value))
    end
    button:SetChecked('true')
    assert(button:GetChecked())
    button:SetChecked(nil)
    assert(not button:GetChecked())
elseif case == 'ranks' then
    SetCVar('ShowAllSpellRanks', 0)
    assert(ShowAllSpellRanksCheckBox:IsVisible(), 'mage rank checkbox is missing')
    local fireTab, frostTab
    for tab = 1, GetNumSpellTabs() do
        local _, _, offset, count, highestOffset, highestCount = GetSpellTabInfo(tab)
        local fire, frost = {}, {}
        for slot = offset + 1, offset + count do
            local _, id = GetSpellBookItemInfo(slot, BOOKTYPE_SPELL)
            if id == 133 or id == 143 or id == 145 then table.insert(fire, slot) end
            if id == 116 or id == 205 then table.insert(frost, slot) end
        end
        if #fire > 0 then
            fireTab = tab
            assert(#fire == 3, 'all learned Fireball ranks must remain accessible')
            assert(highestCount == 1, 'Fireball ranks were not collapsed')
            local slot = GetKnownSlotFromHighestRankSlot(highestOffset + 1)
            local _, id = GetSpellBookItemInfo(slot, BOOKTYPE_SPELL)
            assert(id == 145, 'filtered Fireball must use learned rank 3')
            local name, rank = GetSpellName(slot, BOOKTYPE_SPELL)
            assert(rank == 'Rank 3', 'rank label was lost')
            local itemName, itemRank = GetSpellBookItemName(slot, BOOKTYPE_SPELL)
            assert(name == itemName and rank == itemRank, 'spellbook APIs disagree')
        end
        if #frost > 0 then
            frostTab = tab
            assert(#frost == 2)
            assert(highestCount == count - 1, 'Frostbolt ranks were not collapsed')
            local found = false
            for display = highestOffset + 1, highestOffset + highestCount do
                local slot = GetKnownSlotFromHighestRankSlot(display)
                local _, id = GetSpellBookItemInfo(slot, BOOKTYPE_SPELL)
                assert(id ~= 116, 'lower Frostbolt rank leaked into filtered book')
                if id == 205 then found = true end
            end
            assert(found, 'filtered offsets lost the highest Frostbolt rank')
        end
    end
    assert(fireTab and frostTab and fireTab ~= frostTab, 'class spells need their own skill tabs')
    SpellBookSkillLineTab_OnClick(nil, fireTab)
    UpdateSpells()
    assert(SpellButton1SubSpellName:GetText() == 'Rank 3')
    ShowAllSpellRanksCheckBox:Click()
    assert(GetCVarBool('ShowAllSpellRanks'), 'click must enable all ranks')
    local _, _, _, shown = SpellBook_GetTabInfo(fireTab)
    assert(shown == 3)
    assert(SpellButton1SubSpellName:GetText() == 'Rank 1')
    ShowAllSpellRanksCheckBox:Click()
    assert(not GetCVarBool('ShowAllSpellRanks'), 'second click must restore highest ranks')
    assert(SpellButton1SubSpellName:GetText() == 'Rank 3')
elseif case == 'paging' then
    SetCVar('ShowAllSpellRanks', 1)
    local fireTab
    for tab = 1, GetNumSpellTabs() do
        local _, _, offset, count = GetSpellTabInfo(tab)
        for slot = offset + 1, offset + count do
            local _, id = GetSpellBookItemInfo(slot, BOOKTYPE_SPELL)
            if id == 133 then fireTab = tab end
        end
    end
    assert(fireTab)
    SpellBookSkillLineTab_OnClick(nil, fireTab)
    UpdateSpells()
    SpellBookNextPageButton:Click()
    local page = SpellBook_GetCurrentPage()
    assert(page == 2, 'all ranks should occupy a second page')
    ShowAllSpellRanksCheckBox:Click()
    local filteredPage, pages = SpellBook_GetCurrentPage()
    assert(filteredPage == 1 and pages == 1, 'rank filtering must clamp the current page')
    local slot = SpellBook_GetSpellID(1)
    local _, id = GetSpellBookItemInfo(slot, BOOKTYPE_SPELL)
    assert(id == 42833, 'page clamp must display the highest learned rank')
    assert(SpellButton1SubSpellName:GetText() == 'Rank 16')
    PickupSpell(slot, BOOKTYPE_SPELL)
    local kind, _, _, cursorId = GetCursorInfo()
    assert(kind == 'spell' and cursorId == 42833, 'dragging a filtered spell must pick its actual rank')
    ClearCursor()
elseif case == 'contents' then
    local foundAttack, foundProfession, foundPassive = false, false, false
    for tab = 1, GetNumSpellTabs() do
        local _, _, offset, count = GetSpellTabInfo(tab)
        for slot = offset + 1, offset + count do
            local _, id = GetSpellBookItemInfo(slot, BOOKTYPE_SPELL)
            assert(id ~= 11069, 'hidden talent appeared in the book')
            assert(id ~= 2385, 'crafting recipe appeared in the book')
            if id == 6603 then foundAttack = true end
            if id == 3908 then foundProfession = true
                assert(tab == 1, 'profession opener belongs in General') end
            if id == 20598 then foundPassive = true end
        end
    end
    assert(foundAttack and foundProfession and foundPassive, 'visible abilities must remain in the book')
elseif case == 'warrior' then
    assert(not ShowAllSpellRanksCheckBox:IsVisible(), 'original warrior book has no rank checkbox')
else
    error('unknown spellbook test case')
end
