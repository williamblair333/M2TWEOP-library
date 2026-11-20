///
//![Lua logo](../Lua.png)
//@module LuaPlugin
//@author Fynn
//@license GPL-3.0
#include "pch.h"
#include "gameUi.h"

#include "campaign.h"
#include "gameStringHelpers.h"
#include "functionsOffsets.h"
#include "gameHelpers.h"
#include "settlement.h"
#include "fort.h"
#include "character.h"
#include "dataOffsets.h"
#include "unit.h"
#include "luaPlugin.h"

namespace gameUiHelpers
{
	
	settlementInfoScroll* getSettlementInfoScroll()
	{
		const auto ui = getStratUi();
		if (!ui) 
			return nullptr;
		return ui->settlementInfoScroll;
	}

	uiCardManager* getUiCardManager()
	{
		return gameHelpers::getGameDataAll()->uiCardManager;
	}

	// Bytes readable at p, up to n (0 if p is not committed, readable memory). A closed window's
	// parts stay on the game's lists with stale pointers: a sub-element named "null" under
	// siege_scroll pointed at freed memory and reading it crashed the game (Parley, 11-12).
	static size_t readableBytes(const void* p, const size_t n)
	{
		if (!p)
			return 0;
		MEMORY_BASIC_INFORMATION m{};
		if (VirtualQuery(p, &m, sizeof(m)) == 0 || m.State != MEM_COMMIT)
			return 0;
		if ((m.Protect & 0xff) == PAGE_NOACCESS || (m.Protect & PAGE_GUARD))
			return 0;
		const auto end = reinterpret_cast<uintptr_t>(m.BaseAddress) + m.RegionSize;
		const auto avail = end - reinterpret_cast<uintptr_t>(p);
		return avail < n ? avail : n;
	}

	static bool isReadable(const void* p, const size_t n)
	{
		return readableBytes(p, n) == n;
	}

	// the part of a uiElement read here: name, position, parent, children and their count
	static constexpr size_t uiElementReadSize = offsetof(uiElement, subElementsNum) + sizeof(int);

	std::string getUIElementName(const uiElement* element)
	{
		if (!isReadable(element, uiElementReadSize) || !element->elementName)
			return std::string("");
		const size_t avail = readableBytes(element->elementName, 256);
		if (avail == 0)
			return std::string("");
		return std::string(element->elementName, strnlen(element->elementName, avail));
	}

	// nullptr for an index out of range or a child whose memory can't be read (never a bad pointer)
	uiElement* getSubElement(const uiElement* element, const int index)
	{
		if (!isReadable(element, uiElementReadSize) || index < 0 || index >= element->subElementsNum
			|| element->subElementsNum > 1000)
			return nullptr;
		if (!isReadable(element->subElements + index, sizeof(uiElement*)))
			return nullptr;
		uiElement* child = element->subElements[index];
		return isReadable(child, uiElementReadSize) ? child : nullptr;
	}

	bool useButton(const char* buttonName)
	{
		uiElement* foundButton = getUiElement(buttonName);
		if (!foundButton)
			return false;
		useUiElement(foundButton);
		return true;
	}
	
	uiElement* getUiElement(const char* elementName)
	{
		const auto hashedName = gameStringHelpers::createHashedStringGame(elementName);
		uiElement* resElement = GAME_FUNC(uiElement*(__cdecl*)(stringWithHash*), getUiElementFunc)(hashedName);
		gameStringHelpers::freeHashString(hashedName);

		return resElement;
	}

	sol::table getOpenUiElements()
	{
		sol::table res = plugData::data.luaAll.luaState.create_table();
		if (!codes::offsets.uiElementList)
			return res;
		struct uiElementListStruct
		{
			uiElement** list;
			uint16_t capacity;
			uint16_t count;
		};
		const auto arr = reinterpret_cast<const uiElementListStruct*>(codes::offsets.uiElementList);
		if (!isReadable(arr, sizeof(uiElementListStruct)) || arr->count == 0
			|| !isReadable(arr->list, sizeof(uiElement*)))
			return res;
		const uiElement* root = arr->list[0];
		if (!root || getUIElementName(root) != "strat_ui")
			return res;
		// A shown window hangs under strat_ui (finance_scroll's parent is strat_ui while it is up);
		// a closed one keeps its parts on the list but is cut loose (parent nullptr). So the windows
		// on screen are strat_ui's children, bar the campaign HUD itself.
		if (!root->subElements || root->subElementsNum < 1 || root->subElementsNum > 200)
			return res;
		for (int i = 0; i < root->subElementsNum; i++)
		{
			uiElement* window = getSubElement(root, i);
			if (!window)
				continue;
			if (getUIElementName(window) == "campaign_hud")
				continue;
			res.add(window);
		}
		// A message scroll (a marriage offer, a suitor, any event message opened from its icon) hangs
		// from its message icon at the screen's edge, a root of its own (parent nullptr), not from
		// strat_ui, and the icon doesn't list it among its children (Parley 11-15: a suitor offer,
		// 512x593, under a 47x47 icon). So a window is also any element on the list whose parent is a
		// root other than strat_ui.
		if (arr->count > arr->capacity || !isReadable(arr->list, arr->count * sizeof(uiElement*)))
			return res;
		for (int i = 1; i < arr->count; i++)
		{
			uiElement* e = arr->list[i];
			if (!isReadable(e, uiElementReadSize))
				continue;
			const uiElement* parent = e->parent;
			if (!parent || parent == root || !isReadable(parent, uiElementReadSize) || parent->parent)
				continue;
			res.add(e);
		}
		return res;
	}

	sol::table getRootUiElements()
	{
		// Every root on the element list bar strat_ui (parent nullptr): the minimized-message icons
		// at the screen's left edge (47x47, Parley 11-15), which getOpenUiElements leaves out, and the
		// parts of closed windows, which are cut loose the same way. Every read is checked first: a
		// raw walk of this list from Lua (LuaJIT ffi, Parley 11-19) crashed the game on a freed entry.
		sol::table res = plugData::data.luaAll.luaState.create_table();
		if (!codes::offsets.uiElementList)
			return res;
		struct uiElementListStruct
		{
			uiElement** list;
			uint16_t capacity;
			uint16_t count;
		};
		const auto arr = reinterpret_cast<const uiElementListStruct*>(codes::offsets.uiElementList);
		if (!isReadable(arr, sizeof(uiElementListStruct)) || arr->count == 0 || arr->count > arr->capacity
			|| !isReadable(arr->list, arr->count * sizeof(uiElement*)))
			return res;
		for (int i = 1; i < arr->count; i++)
		{
			uiElement* e = arr->list[i];
			if (!isReadable(e, uiElementReadSize) || e->parent)
				continue;
			res.add(e);
		}
		return res;
	}

	std::string readUiElementBytes(const uiElement* element, const int offset, const int count)
	{
		if (!element || offset < 0 || count <= 0 || count > 1024)
			return std::string("");
		const auto start = reinterpret_cast<const uint8_t*>(element) + offset;
		const size_t n = readableBytes(start, static_cast<size_t>(count));
		static const char digits[] = "0123456789abcdef";
		std::string hex;
		hex.reserve(n * 2);
		for (size_t i = 0; i < n; i++)
		{
			hex += digits[start[i] >> 4];
			hex += digits[start[i] & 0xf];
		}
		return hex;
	}

	void useUiElement(uiElement* element)
	{
		GAME_FUNC(void(__thiscall*)(uiElement*), useButtonFunc)(element);
	}

	// UI::UI_ELEMENT's virtual slot 60 (vtable + 0xF0) is "use this element": bool __thiscall(), no
	// arguments. Every UI_ELEMENT vtable has at least 90 slots. For UI::UI_BUTTON the slot is
	// useButtonFunc itself; a minimized message icon (UI::UI_MESSAGE_ICON) overrides it to open its
	// message. Calling useButtonFunc on an icon jumps through button-only slot 98, past the icon's
	// 98-slot vtable, and crashes the game (Parley, 11-19 and 11-20).
	bool activateUiElement(uiElement* element)
	{
		constexpr size_t useSlot = 60;
		if (!isReadable(element, sizeof(void*)))
			return false;
		const auto vtable = *reinterpret_cast<void* const* const*>(element);
		if (!isReadable(vtable + useSlot, sizeof(void*)))
			return false;
		using useFunc = bool(__thiscall*)(uiElement*);
		const auto use = reinterpret_cast<useFunc>(vtable[useSlot]);
		if (!use)
			return false;
		return use(element);
	}

	stratUIStruct* getStratUi()
	{
		const auto uiPtr = reinterpret_cast<stratUIStruct**>(dataOffsets::offsets.uiStratUiV2);
		if (!uiPtr)
			return nullptr;
		return *uiPtr;
	}

	buildingInfoScroll* getBuildingInfoScroll()
	{
		const auto ui = getStratUi();
		if (!ui) 
			return nullptr;
		return ui->buildingInfoScroll;
	}

	void removeToolTips(const uiElement* unitView)
	{
		const auto localFacId = campaignHelpers::getLocalFactionID();
		for (int i = 0; i < unitView->subElementsNum; i++)
		{
			const auto card = unitView->subElements[i];
			const auto unitCard = reinterpret_cast<uiUnitCard*>(card);
			if (const auto unit = unitCard->unit; unit && unit->spyingInfoUnit.spyingInfoFactionArray[localFacId] < 1)
			{
				card->xSize = 1;
				card->ySize = 1;
			}
		}
	}
	
	void removeToolTipsSett()
	{
		const auto ui = getStratUi();
		if (!ui) 
			return;
		const auto settScroll = ui->otherSettScroll;
		if (!settScroll)
			return;
		removeToolTips(settScroll->unitView);
	}
	
	void removeToolTipsArmy()
	{
		const auto ui = getStratUi();
		if (!ui) 
			return;
		const auto otherCharScroll = ui->otherCharScroll;
		if (!otherCharScroll)
			return;
		removeToolTips(otherCharScroll->unitView);
	}
	
	void removeToolTipsFort()
	{
		const auto ui = getStratUi();
		if (!ui) 
			return;
		const auto fortScroll = ui->fortScroll;
		if (!fortScroll)
			return;
		removeToolTips(fortScroll->unitView);
	}

	void checkNeedRemoveTooltips()
	{
		if (!stratMapHelpers::isStratMap())
			return;
		removeToolTipsSett();
		removeToolTipsArmy();
		removeToolTipsFort();
	}

	unitInfoScroll* getUnitInfoScroll()
	{
		const auto ui = getStratUi();
		if (!ui) 
			return nullptr;
		return ui->unitInfoScroll;
	}

	uiFamilyTree* getFamilyTree()
	{
		const auto ui = getStratUi();
		if (!ui) 
			return nullptr;
		const auto familyScroll = ui->familyTreeScroll;
		if (!familyScroll)
			return nullptr;
		return &familyScroll->familyTree;
	}
	
    void addToLua(sol::state& luaState)
    {
        struct
        {
            sol::table gameUITable;
        }tables;
        
        struct
        {
            sol::usertype<uiElement>uiElement;
            sol::usertype<settlementInfoScroll>settlementInfoScroll;
            sol::usertype<settlementTextStrings>settlementTextStrings;
        	sol::usertype<uiString>uiString;
        	sol::usertype<uiCardManager> uiCardManager;
        	sol::usertype<buildingInfoScroll> buildingInfoScroll;
        	sol::usertype<unitInfoScroll> unitInfoScroll;
        	sol::usertype<uiFamilyTree> uiFamilyTree;
        	sol::usertype<uiFamilyLeaf> uiFamilyLeaf;
        }types;

		///GameUI
		//@section GameUI

		/***
		Basic gameSTDUI table

		@tfield getUiElement getUiElement
		@tfield getOpenUiElements getOpenUiElements

		@table gameSTDUI
		*/
		
		tables.gameUITable = luaState.create_table("gameSTDUI");
		/***
		Get a game UI element, element must be opened.
		@function gameSTDUI.getUiElement
		@tparam string elementName
		@treturn uiElementStruct element
		@usage
		function analyzeScroll(scrollName)
			local scroll, uiList = gameSTDUI.getUiElement(scrollName), "UI elements of scroll: "..scrollName
			if scroll ~= nil then
				for i = 0, scroll.subElementsNum - 1, 1 do
					uiList = uiList.."\n\t"..i.." "..scroll:getSubElement(i).elementName
				end
				print(uiList)
			end
		end
		*/

		tables.gameUITable.set_function("getUiElement", &getUiElement);

		/***
		Get the windows on screen now: scrolls and panels hung under strat_ui (bar the campaign HUD), and open message scrolls (offers, event messages) hung from their message icon.
		@function gameSTDUI.getOpenUiElements
		@treturn table elements
		@usage
		local openElements = gameSTDUI.getOpenUiElements()
		for i = 1, #openElements do
			print(openElements[i].elementName)
		end
		*/

		tables.gameUITable.set_function("getOpenUiElements", &getOpenUiElements);

		/***
		Get every root UI element bar strat_ui (no parent): the minimized-message icons at the screen's left edge, and the parts of closed windows. Every pointer is checked before it is read.
		@function gameSTDUI.getRootUiElements
		@treturn table elements
		@usage
		for _, e in ipairs(gameSTDUI.getRootUiElements()) do
			if e.xSize < 64 and e.xPos < 64 then print("message icon at", e.xPos, e.yPos) end
		end
		*/

		tables.gameUITable.set_function("getRootUiElements", &getRootUiElements);

		/// UIElement
		//@section uiElementStructTable

		/***
		Basic uiElementStruct table

		@tfield string elementName
		@tfield int xSize
		@tfield int ySize
		@tfield int xPos
		@tfield int yPos
		@tfield int subElementsNum
		@tfield getSubElement getSubElement
		@tfield execute execute

		@table uiElementStruct
		*/
		types.uiElement = luaState.new_usertype<uiElement>("uiElementStruct");
		types.uiElement.set("elementName", sol::property(
			&getUIElementName));
		types.uiElement.set("xSize", &uiElement::xSize);
		types.uiElement.set("ySize", &uiElement::ySize);
		types.uiElement.set("xPos", &uiElement::xPos);
		types.uiElement.set("yPos", &uiElement::yPos);
		types.uiElement.set("subElementsNum", &uiElement::subElementsNum);
		/***
		Get a subelement of an UI element using the index.
		@function uiElementStruct:getSubElement
		@tparam int index Starts from 0.
		@treturn uiElementStruct subelement
		@usage
		local financeScroll = gameSTDUI.getUiElement("finance_scroll");
		local subElement1 = financeScroll:getSubElement(5);
		*/
		types.uiElement.set("getSubElement", &getSubElement);
		/***
		execute standard game UI element, use only for buttons
		@function uiElementStruct:execute
		@usage
		local financeScroll = gameSTDUI.getUiElement("finance_scroll");
		--at index 5 we have faction_listviews_button
		local subElement1 = financeScroll:getSubElement(5);
		subElement1:execute();
		*/
		types.uiElement.set("execute", &useUiElement);
		/***
		Use the element the way the game does when it is clicked: its own virtual "use" function, so a button presses and a minimized message icon opens its message. Unlike execute, it is safe on elements that are not buttons.
		@function uiElementStruct:activate
		@treturn bool what the element's use function returned (false if the element can't be read)
		@usage
		for _, e in ipairs(gameSTDUI.getRootUiElements()) do
			if e.xSize == 47 and e.ySize == 47 then e:activate() break end
		end
		*/
		types.uiElement.set("activate", &activateUiElement);
		/***
		The element's own bytes as hex, for working out what its fields mean (a live message icon against a cleared one, a button against a panel). Only memory that can be read is read; the result is shorter where it stops.
		@function uiElementStruct:readBytes
		@tparam int offset From the element's start, 0 or more.
		@tparam int count At most 1024.
		@treturn string hex Two characters a byte, "" when nothing can be read.
		@usage
		print(gameSTDUI.getRootUiElements()[1]:readBytes(0, 256))
		*/
		types.uiElement.set("readBytes", &readUiElementBytes);
		
		/***
		Basic settlementTextStrings table.

		@tfield uiString incomeString
		@tfield uiString incomeValue
		@tfield uiString publicOrderString
		@tfield uiString publicOrderValue
		@tfield uiString populationString
		@tfield uiString populationValue
		@tfield uiString populationGrowthString
		@tfield uiString populationGrowthValue

		@table settlementTextStrings
		*/
		types.settlementTextStrings = luaState.new_usertype<settlementTextStrings>("settlementTextStrings");
		types.settlementTextStrings.set("incomeString", &settlementTextStrings::incomeString);
		types.settlementTextStrings.set("incomeValue", &settlementTextStrings::incomeValue);
		types.settlementTextStrings.set("publicOrderString", &settlementTextStrings::publicOrderString);
		types.settlementTextStrings.set("publicOrderValue", &settlementTextStrings::publicOrderValue);
		types.settlementTextStrings.set("populationString", &settlementTextStrings::populationString);
		types.settlementTextStrings.set("populationValue", &settlementTextStrings::populationValue);
		types.settlementTextStrings.set("populationGrowthString", &settlementTextStrings::populationGrowthString);
		types.settlementTextStrings.set("populationGrowthValue", &settlementTextStrings::populationGrowthValue);
		
		/***
		Basic uiString table.

		@tfield int thickness (0-255)
		@tfield int blue (0-255)
		@tfield int green (0-255)
		@tfield int red (0-255)

		@table uiString
		*/
		types.uiString = luaState.new_usertype<uiString>("uiString");
		types.uiString.set("thickness", &uiString::thickness);
		types.uiString.set("blue", &uiString::blue);
		types.uiString.set("green", &uiString::green);
		types.uiString.set("red", &uiString::red);

		/***
		Basic settlementInfoScroll table.

		@tfield settlementStruct settlement
		@tfield getUIStrings getUIStrings

		@table settlementInfoScroll
		*/
		types.settlementInfoScroll = luaState.new_usertype<settlementInfoScroll>("settlementInfoScroll");
		types.settlementInfoScroll.set("settlement", &settlementInfoScroll::settlement);

		/***
		Get settlement text color info.
		@function settlementInfoScroll:getUIStrings
		@treturn settlementTextStrings getUIStrings
		@usage
		function onScrollOpened(eventData)
    		local settlementInfoScroll = M2TWEOP.getSettlementInfoScroll();
			if settlementInfoScroll then
				settlementInfoScroll:getUIStrings().incomeString.red = 0
				settlementInfoScroll:getUIStrings().incomeString.green = 0
				settlementInfoScroll:getUIStrings().incomeString.blue = 0
				settlementInfoScroll:getUIStrings().incomeString.thickness = 0

				settlementInfoScroll:getUIStrings().incomeValue.red = 0
				settlementInfoScroll:getUIStrings().incomeValue.green = 0
				settlementInfoScroll:getUIStrings().incomeValue.blue = 0
			end
		end
		*/
		types.settlementInfoScroll.set_function("getUIStrings", &settlementInfoScroll::getUIStrings);

		///UI Card Manager
		//@section UI Card Manager

		/***
		Basic uiCardManager table

		@tfield int selectedUnitCardsCount
		@tfield int unitCardsCount
		@tfield settlementStruct selectedSettlement
		@tfield character selectedCharacter
		@tfield fortStruct selectedFort
		@tfield getSelectedUnitCard getSelectedUnitCard
		@tfield getUnitCard getUnitCard
		@tfield getBuildingInfoScroll getBuildingInfoScroll
		@tfield getUnitInfoScroll getUnitInfoScroll
		@tfield getFamilyTree getFamilyTree

		@table uiCardManager
		*/
		types.uiCardManager = luaState.new_usertype<uiCardManager>("uiCardManager");
		types.uiCardManager.set("selectedUnitCardsCount", &uiCardManager::selectedUnitCardsCount);
		types.uiCardManager.set("unitCardsCount", &uiCardManager::unitCardsCount);
		types.uiCardManager.set("selectedSettlement", &uiCardManager::selectedSettlement);
		types.uiCardManager.set("selectedCharacter", &uiCardManager::selectedCharacter);
		types.uiCardManager.set("selectedFort", &uiCardManager::selectedFort);
		
		/***
		Get selected unit card by index.
		@function uiCardManager:getSelectedUnitCard
		@tparam int index
		@treturn unit selectedUnit
		@usage
		local cardManager = M2TW.uiCardManager
		local selectedUnit=cardManager:getSelectedUnitCard(0)
		*/
		types.uiCardManager.set_function("getSelectedUnitCard", &uiCardManager::getSelectedUnitCard);
		/***
		Get unit card by index (battle or strat).
		@function uiCardManager:getUnitCard
		@tparam int index
		@treturn unit unit
		@usage
		local cardManager = M2TW.uiCardManager
		local unit = cardManager:getUnitCard(0)
		*/
		types.uiCardManager.set_function("getUnitCard", &uiCardManager::getUnitCard);
		/***
		Get building info scroll.
		@function uiCardManager.getBuildingInfoScroll
		@treturn buildingInfoScroll scroll
		@usage
		local infoScroll = M2TW.uiCardManager.getBuildingInfoScroll()
		*/
		types.uiCardManager.set_function("getBuildingInfoScroll", &getBuildingInfoScroll);
		/***
		Get unit info scroll.
		@function uiCardManager.getUnitInfoScroll
		@treturn unitInfoScroll scroll
		@usage
		local infoScroll = M2TW.uiCardManager.getUnitInfoScroll()
		*/
		types.uiCardManager.set_function("getUnitInfoScroll", &getUnitInfoScroll);
		/***
		Get family tree from scroll. Make sure scroll is open!
		@function uiCardManager.getFamilyTree
		@treturn uiFamilyTree familyTree
		@usage
		local familyTree = M2TW.uiCardManager.getFamilyTree()
		*/
		types.uiCardManager.set_function("getFamilyTree", &getFamilyTree);
		
		/***
		Basic unitInfoScroll table

		@tfield unit unit If the scroll is about existing unit
		@tfield eduEntry eduEntry
		@tfield unitInQueue recruitmentItem
		@tfield mercPoolUnit mercenary

		@table unitInfoScroll
		*/
		types.unitInfoScroll = luaState.new_usertype<unitInfoScroll>("unitInfoScroll");
		types.unitInfoScroll.set("unit", &unitInfoScroll::unit);
		types.unitInfoScroll.set("eduEntry", sol::property(&unitInfoScroll::getEntry));
		types.unitInfoScroll.set("recruitmentItem", &unitInfoScroll::recruitmentItem);
		types.unitInfoScroll.set("mercenary", &unitInfoScroll::mercenary);

		/***
		Basic buildingInfoScroll table

		@tfield settlementStruct settlement
		@tfield building building
		@tfield edbEntry edbEntry
		@tfield buildingLevel level

		@table buildingInfoScroll
		*/
		types.buildingInfoScroll = luaState.new_usertype<buildingInfoScroll>("buildingInfoScroll");
		types.buildingInfoScroll.set("settlement", &buildingInfoScroll::settlement);
		types.buildingInfoScroll.set("building", &buildingInfoScroll::building);
		types.buildingInfoScroll.set("edbEntry", sol::property(&buildingInfoScroll::getEdbEntry));
		types.buildingInfoScroll.set("level", sol::property(&buildingInfoScroll::getLevel));

		/***
		Basic uiFamilyLeaf table

		@tfield characterRecord record
		@tfield int generation
		@tfield uiFamilyLeaf parent
		@tfield uiFamilyLeaf spouse
		@tfield int numChildren
		@tfield getChild getChild

		@table uiFamilyLeaf
		*/
		types.uiFamilyLeaf = luaState.new_usertype<uiFamilyLeaf>("uiFamilyLeaf");
		types.uiFamilyLeaf.set("record", &uiFamilyLeaf::record);
		types.uiFamilyLeaf.set("generation", &uiFamilyLeaf::generation);
		types.uiFamilyLeaf.set("parent", &uiFamilyLeaf::parent);
		types.uiFamilyLeaf.set("spouse", &uiFamilyLeaf::spouse);
		types.uiFamilyLeaf.set("numChildren", &uiFamilyLeaf::numChildren);
		
		/***
		Get a child.
		@function uiFamilyLeaf:getChild
		@tparam int index
		@treturn uiFamilyLeaf child
		@usage
		local child = uiFamilyLeaf:getChild(0)
		*/
		types.uiFamilyLeaf.set_function("getChild", &uiFamilyLeaf::getChild);

		/***
		Basic uiFamilyTree table

		@tfield uiFamilyLeaf familyRoot
		@tfield int generations
		@tfield uiFamilyLeaf hoveredLeaf
		@tfield uiFamilyLeaf selectedLeaf
		@tfield bool canSelectAll

		@table uiFamilyTree
		*/
		types.uiFamilyTree = luaState.new_usertype<uiFamilyTree>("uiFamilyTree");
		types.uiFamilyTree.set("familyRoot", &uiFamilyTree::familyRoot);
		types.uiFamilyTree.set("generations", &uiFamilyTree::generations);
		types.uiFamilyTree.set("hoveredLeaf", &uiFamilyTree::hoveredLeaf);
		types.uiFamilyTree.set("selectedLeaf", &uiFamilyTree::selectedLeaf);
		types.uiFamilyTree.set("canSelectAll", &uiFamilyTree::canSelectAll);
    }
}

buildingLevel* buildingInfoScroll::getLevel()
{
	if (level)
		return level;
	if (entry && settlement)
	{
		int8_t lvl = -1;
		for (int i = 0; i < settlement->buildingsNum; i++)
		{
			if (settlement->getBuilding(i)->edbEntry == entry)
			{
				lvl = settlement->getBuilding(i)->level;
				break;
			}
		}
		if (lvl == -1)
			return entry->getBuildingLevel(0);
		if ((lvl + 1) < entry->buildingLevelCount)
			return entry->getBuildingLevel(lvl + 1);
		return entry->getBuildingLevel(lvl);
	}
	if (techTreeItem)
	{
		return techTreeItem->getLevel();
	}
	if (building)
	{
		return building->edbEntry->getBuildingLevel(building->level);
	}
	return nullptr;
}

edbEntry* buildingInfoScroll::getEdbEntry()
{
	if (entry)
		return entry;
	if (building)
		return building->edbEntry;
	if (techTreeItem)
		return techTreeItem->entry;
	if (level)
	{
		if (const auto id = buildingHelpers::getBuildingLevelId(level->name); id != -1)
		{
			return eopBuildings::getEdb()->getBuildingByID(id);
		}
	}
	return nullptr;
}

struct eduEntry* unitInfoScroll::getEntry()
{
	if (entry)
		return entry;
	if (unit)
		return unit->eduEntry;
	if (recruitmentItem)
		return recruitmentItem->entry;
	if (mercenary)
		return mercenary->eduEntry;
	return nullptr;
}


buildingLevel* techTreeThreadItem::getLevel()
{
	return entry->getBuildingLevel(level);
}
