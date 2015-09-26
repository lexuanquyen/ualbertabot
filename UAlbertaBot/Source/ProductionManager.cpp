#include "ProductionManager.h"

using namespace UAlbertaBot;

ProductionManager::ProductionManager() 
	: initialBuildSet(false)
	, reservedMinerals(0)
	, reservedGas(0)
	, assignedWorkerForThisBuilding(false)
	, haveLocationForThisBuilding(false)
	, enemyCloakedDetected(false)
	, rushDetected(false)
{
	populateTypeCharMap();

	if (!Config::Modules::UsingBuildOrderDemo)
	{
        setBuildOrder(StrategyManager::Instance().getOpeningBookBuildOrder());
		_runningOpeningBook = true;
	}
}

void ProductionManager::setBuildOrder(const BuildOrder & buildOrder)
{
	_runningOpeningBook = false;
	// clear the current build order
	queue.clearAll();

	// for each item in the results build order, add it
	for (size_t i(0); i<buildOrder.size(); ++i)
	{
		// queue the item
		queue.queueAsLowestPriority(buildOrder[i], true);
	}
}
void ProductionManager::performBuildOrderSearch(const std::vector<MetaPair> & goal)
{
	if (!Config::Modules::UsingBuildOrderSearch || goal.empty())
	{
		return;
	}

	//std::vector<MetaType> buildOrder = BOSSManager::GetOptimizedNaiveBuildOrder(goal);
	//setBuildOrder(buildOrder);

	BuildOrder & buildOrder = BOSSManager::Instance().getBuildOrder();

	if (buildOrder.size() > 0)
	{
		setBuildOrder(buildOrder);
		BOSSManager::Instance().reset();
		searchGoal = std::vector<MetaPair>();
	}
	else
	{
		if (!BOSSManager::Instance().isSearchInProgress())
		{
            if (Config::Debug::DrawBuildOrderSearchInfo)
            {
			    BWAPI::Broodwar->printf("Starting a new build order search!");
            }
			BOSSManager::Instance().startNewSearch(goal);
		}
	}
}
void ProductionManager::performBuildOrderSearch()
{	
    if (!Config::Modules::UsingBuildOrderSearch)
    {
        return;
    }

	BuildOrder & buildOrder = BOSSManager::Instance().getBuildOrder();

    if (buildOrder.size() > 0)
    {
	    setBuildOrder(buildOrder);
        BOSSManager::Instance().reset();
        searchGoal = std::vector<MetaPair>();
    }
    else
    {
        if (!BOSSManager::Instance().isSearchInProgress())
        {
            if (Config::Debug::DrawBuildOrderSearchInfo)
            {
			    //BWAPI::Broodwar->printf("Starting a new build order search!");
            }

			BOSSManager::Instance().startNewSearch(StrategyManager::Instance().getBuildOrderGoal());
        }
    }
}

void ProductionManager::setSearchGoal(MetaPairVector & goal)
{
	searchGoal = goal;
}

void ProductionManager::update() 
{
	// check the queue for stuff we can build
	manageBuildOrderQueue();

    // build order demo only
	if (Config::Modules::UsingBuildOrderDemo && (queue.size() == 0))
	{
		performBuildOrderSearch(searchGoal);
	}

	// if nothing is currently building, get a new goal from the strategy manager
	if ((queue.size() == 0) && (BWAPI::Broodwar->getFrameCount() > 10) && !Config::Modules::UsingBuildOrderDemo)
	{
        if (Config::Debug::DrawBuildOrderSearchInfo)
        {
		    BWAPI::Broodwar->drawTextScreen(150, 10, "Nothing left to build, new search!");
        }
		performBuildOrderSearch();
	}

	//// detect if there's a build order deadlock once per second
	if ((BWAPI::Broodwar->getFrameCount() % 24 == 0) && detectBuildOrderDeadlock())
	{
        if (Config::Debug::DrawBuildOrderSearchInfo)
        {
		    BWAPI::Broodwar->printf("Supply deadlock detected, building supply!");
        }
		queue.queueAsHighestPriority(MetaType(BWAPI::Broodwar->self()->getRace().getSupplyProvider()), true);
	}

	// if they have cloaked units get a new goal asap
	if (BWAPI::Broodwar->self()->getRace() == BWAPI::Races::Protoss && !enemyCloakedDetected && InformationManager::Instance().enemyHasCloakedUnits())
	{
		if (BWAPI::Broodwar->self()->allUnitCount(BWAPI::UnitTypes::Protoss_Photon_Cannon) < 2)
		{
			queue.queueAsHighestPriority(MetaType(BWAPI::UnitTypes::Protoss_Photon_Cannon), true);
			queue.queueAsHighestPriority(MetaType(BWAPI::UnitTypes::Protoss_Photon_Cannon), true);
		}

		if (BWAPI::Broodwar->self()->allUnitCount(BWAPI::UnitTypes::Protoss_Forge) == 0)
		{
			queue.queueAsHighestPriority(MetaType(BWAPI::UnitTypes::Protoss_Forge), true);
		}

        if (Config::Debug::DrawBuildOrderSearchInfo)
        {
		    BWAPI::Broodwar->printf("Enemy Cloaked Unit Detected!");
        }
		enemyCloakedDetected = true;
	}
}

// on unit destroy
void ProductionManager::onUnitDestroy(BWAPI::Unit unit)
{
	// we don't care if it's not our unit
	if (!unit || unit->getPlayer() != BWAPI::Broodwar->self())
	{
		return;
	}
		
	if (Config::Modules::UsingBuildOrderSearch)
	{
		// if it's a worker or a building, we need to re-search for the current goal
		if ((unit->getType().isWorker() && !WorkerManager::Instance().isWorkerScout(unit)) || unit->getType().isBuilding())
		{
			if (unit->getType() != BWAPI::UnitTypes::Zerg_Drone)
			{
                if (Config::Debug::DrawBuildOrderSearchInfo)
                {
			        BWAPI::Broodwar->printf("Critical unit died, re-searching build order");
                }
				performBuildOrderSearch(StrategyManager::Instance().getBuildOrderGoal());
			}
		}
	}
}

void ProductionManager::manageBuildOrderQueue() 
{
	// if there is nothing in the queue, oh well
	if (queue.isEmpty()) 
	{
		return;
	}

	// the current item to be used
	BuildOrderItem & currentItem = queue.getHighestPriorityItem();

	// while there is still something left in the queue
	while (!queue.isEmpty()) 
	{
		// this is the unit which can produce the currentItem
        BWAPI::Unit producer = getProducer(currentItem.metaType);

		// check to see if we can make it right now
		bool canMake = canMakeNow(producer, currentItem.metaType);

		// if we try to build too many refineries manually remove it
		if (currentItem.metaType.isRefinery() && (BWAPI::Broodwar->self()->allUnitCount(BWAPI::Broodwar->self()->getRace().getRefinery() >= 3)))
		{
			queue.removeCurrentHighestPriorityItem();
			break;
		}

		// if the next item in the list is a building and we can't yet make it
        if (currentItem.metaType.isBuilding() && !(producer && canMake) && !currentItem.metaType.getUnitType().isAddon())
		{
			// construct a temporary building object
			Building b(currentItem.metaType.getUnitType(), BWAPI::Broodwar->self()->getStartLocation());
            b.isGasSteal = currentItem.isGasSteal;

			// set the producer as the closest worker, but do not set its job yet
			producer = WorkerManager::Instance().getBuilder(b, false);

			// predict the worker movement to that building location
			predictWorkerMovement(b);
		}

		// if we can make the current item
		if (producer && canMake) 
		{
			// create it
			create(producer, currentItem);
			assignedWorkerForThisBuilding = false;
			haveLocationForThisBuilding = false;

			// and remove it from the queue
			queue.removeCurrentHighestPriorityItem();

			// don't actually loop around in here
			break;
		}
		// otherwise, if we can skip the current item
		else if (queue.canSkipItem())
		{
			// skip it
			queue.skipItem();

			// and get the next one
			currentItem = queue.getNextHighestPriorityItem();				
		}
		else 
		{
			// so break out
			break;
		}
	}
}

BWAPI::Unit ProductionManager::getProducer(MetaType t, BWAPI::Position closestTo)
{
    // get the type of unit that builds this
    BWAPI::UnitType producerType = t.whatBuilds();

    // make a set of all candidate producers
    BWAPI::Unitset candidateProducers;
    for (auto & unit : BWAPI::Broodwar->self()->getUnits())
    {
        UAB_ASSERT(unit != nullptr, "Unit was null");

        // reasons a unit can not train the desired type
        if (unit->getType() != producerType)                    { continue; }
        if (!unit->isCompleted())                               { continue; }
        if (unit->isTraining())                                 { continue; }
        if (unit->isLifted())                                   { continue; }
        if (!unit->isPowered())                                 { continue; }

        // if the type is an addon, some special cases
        if (t.getUnitType().isAddon())
        {
            // if the unit already has an addon, it can't make one
            if (unit->getAddon() != nullptr)
            {
                continue;
            }

            // if we just told this unit to build an addon, then it will not be building another one
            // this deals with the frame-delay of telling a unit to build an addon and it actually starting to build
            if (unit->getLastCommand().getType() == BWAPI::UnitCommandTypes::Build_Addon) 
            { 
                continue; 
            }

            bool isBlocked = false;

            // if the unit doesn't have space to build an addon, it can't make one
            BWAPI::TilePosition addonPosition(unit->getTilePosition().x + unit->getType().tileWidth(), unit->getTilePosition().y + unit->getType().tileHeight() - t.getUnitType().tileHeight());
            BWAPI::Broodwar->drawBoxMap(addonPosition.x*32, addonPosition.y*32, addonPosition.x*32 + 64, addonPosition.y*32 + 64, BWAPI::Colors::Red);
            
            for (int i=0; i<unit->getType().tileWidth() + t.getUnitType().tileWidth(); ++i)
            {
                for (int j=0; j<unit->getType().tileHeight(); ++j)
                {
                    BWAPI::TilePosition tilePos(unit->getTilePosition().x + i, unit->getTilePosition().y + j);

                    // if the map won't let you build here, we can't build it
                    if (!BWAPI::Broodwar->isBuildable(tilePos))
                    {
                        isBlocked = true;
                        BWAPI::Broodwar->drawBoxMap(tilePos.x*32, tilePos.y*32, tilePos.x*32 + 32, tilePos.y*32 + 32, BWAPI::Colors::Red);
                    }

                    // if there are any units on the addon tile, we can't build it
                    BWAPI::Unitset uot = BWAPI::Broodwar->getUnitsOnTile(tilePos.x, tilePos.y);
                    if (uot.size() > 0 && !(uot.size() == 1 && *(uot.begin()) == unit))
                    {
                        isBlocked = true;;
                        BWAPI::Broodwar->drawBoxMap(tilePos.x*32, tilePos.y*32, tilePos.x*32 + 32, tilePos.y*32 + 32, BWAPI::Colors::Red);
                    }
                }
            }

            if (isBlocked)
            {
                continue;
            }
        }
        
        // if the type requires an addon and the producer doesn't have one
        typedef std::pair<BWAPI::UnitType, int> ReqPair;
        for (const ReqPair & pair : t.getUnitType().requiredUnits())
        {
            BWAPI::UnitType requiredType = pair.first;
            if (requiredType.isAddon())
            {
                if (!unit->getAddon() || (unit->getAddon()->getType() != requiredType))
                {
                    continue;
                }
            }
        }

        // if we haven't cut it, add it to the set of candidates
        candidateProducers.insert(unit);
    }

    return getClosestUnitToPosition(candidateProducers, closestTo);
}

BWAPI::Unit ProductionManager::getClosestUnitToPosition(const BWAPI::Unitset & units, BWAPI::Position closestTo)
{
    if (units.size() == 0)
    {
        return nullptr;
    }

    // if we don't care where the unit is return the first one we have
    if (closestTo == BWAPI::Positions::None)
    {
        return *(units.begin());
    }

    BWAPI::Unit closestUnit = nullptr;
    double minDist(1000000);

	for (auto & unit : units) 
    {
        UAB_ASSERT(unit != nullptr, "Unit was null");

		double distance = unit->getDistance(closestTo);
		if (!closestUnit || distance < minDist) 
        {
			closestUnit = unit;
			minDist = distance;
		}
	}

    return closestUnit;
}

// this function will check to see if all preconditions are met and then create a unit
void ProductionManager::create(BWAPI::Unit producer, BuildOrderItem & item) 
{
    if (!producer)
    {
        return;
    }

    MetaType t = item.metaType;

    // if we're dealing with a building
    if (t.isUnit() && t.getUnitType().isBuilding() 
        && t.getUnitType() != BWAPI::UnitTypes::Zerg_Lair 
        && t.getUnitType() != BWAPI::UnitTypes::Zerg_Hive
        && t.getUnitType() != BWAPI::UnitTypes::Zerg_Greater_Spire
        && !t.getUnitType().isAddon())
    {
        // send the building task to the building manager
        BuildingManager::Instance().addBuildingTask(t.getUnitType(), BWAPI::Broodwar->self()->getStartLocation(), item.isGasSteal);
    }
    else if (t.getUnitType().isAddon())
    {
        //BWAPI::TilePosition addonPosition(producer->getTilePosition().x + producer->getType().tileWidth(), producer->getTilePosition().y + producer->getType().tileHeight() - t.unitType.tileHeight());
        producer->buildAddon(t.getUnitType());
    }
    // if we're dealing with a non-building unit
    else if (t.isUnit()) 
    {
        // if the race is zerg, morph the unit
        if (t.getUnitType().getRace() == BWAPI::Races::Zerg) 
        {
            producer->morph(t.getUnitType());
        // if not, train the unit
        } 
        else 
        {
            producer->train(t.getUnitType());
        }
    }
    // if we're dealing with a tech research
    else if (t.isTech())
    {
        producer->research(t.getTechType());
    }
    else if (t.isUpgrade())
    {
        //Logger::Instance().log("Produce Upgrade: " + t.getName() + "\n");
        producer->upgrade(t.getUpgradeType());
    }
    else
    {	
		
    }
}

bool ProductionManager::canMakeNow(BWAPI::Unit producer, MetaType t)
{
    //UAB_ASSERT(producer != nullptr, "Producer was null");

	bool canMake = meetsReservedResources(t);
	if (canMake)
	{
		if (t.isUnit())
		{
			canMake = BWAPI::Broodwar->canMake(t.getUnitType(), producer);
		}
		else if (t.isTech())
		{
			canMake = BWAPI::Broodwar->canResearch(t.getTechType(), producer);
		}
		else if (t.isUpgrade())
		{
			canMake = BWAPI::Broodwar->canUpgrade(t.getUpgradeType(), producer);
		}
		else
		{	
			UAB_ASSERT(false, "Unknown type");
		}
	}

	return canMake;
}

bool ProductionManager::detectBuildOrderDeadlock()
{
	// if the queue is empty there is no deadlock
	if (queue.size() == 0 || BWAPI::Broodwar->self()->supplyTotal() >= 390)
	{
		return false;
	}

	// are any supply providers being built currently
	bool supplyInProgress =		BuildingManager::Instance().isBeingBuilt(BWAPI::Broodwar->self()->getRace().getCenter()) || 
								BuildingManager::Instance().isBeingBuilt(BWAPI::Broodwar->self()->getRace().getSupplyProvider());

    for (auto & unit : BWAPI::Broodwar->self()->getUnits())
    {
        if (unit->getType() == BWAPI::UnitTypes::Zerg_Egg)
        {
            if (unit->getBuildType() == BWAPI::UnitTypes::Zerg_Overlord)
            {
                supplyInProgress = true;
                break;
            }
        }
    }

	// does the current item being built require more supply
    
	int supplyCost			= queue.getHighestPriorityItem().metaType.supplyRequired();
	int supplyAvailable		= std::max(0, BWAPI::Broodwar->self()->supplyTotal() - BWAPI::Broodwar->self()->supplyUsed());

	// if we don't have enough supply and none is being built, there's a deadlock
	if ((supplyAvailable < supplyCost) && !supplyInProgress)
	{
		return true;
	}

	return false;
}

// When the next item in the queue is a building, this checks to see if we should move to it
// This function is here as it needs to access prodction manager's reserved resources info
void ProductionManager::predictWorkerMovement(const Building & b)
{
    if (b.isGasSteal)
    {
        return;
    }

	// get a possible building location for the building
	if (!haveLocationForThisBuilding)
	{
		predictedTilePosition = BuildingManager::Instance().getBuildingLocation(b);
	}

	if (predictedTilePosition != BWAPI::TilePositions::None)
	{
		haveLocationForThisBuilding = true;
	}
	else
	{
		return;
	}
	
	// draw a box where the building will be placed
	int x1 = predictedTilePosition.x * 32;
	int x2 = x1 + (b.type.tileWidth()) * 32;
	int y1 = predictedTilePosition.y * 32;
	int y2 = y1 + (b.type.tileHeight()) * 32;
	if (Config::Debug::DrawWorkerInfo) 
    {
        BWAPI::Broodwar->drawBoxMap(x1, y1, x2, y2, BWAPI::Colors::Blue, false);
    }

	// where we want the worker to walk to
	BWAPI::Position walkToPosition		= BWAPI::Position(x1 + (b.type.tileWidth()/2)*32, y1 + (b.type.tileHeight()/2)*32);

	// compute how many resources we need to construct this building
	int mineralsRequired				= std::max(0, b.type.mineralPrice() - getFreeMinerals());
	int gasRequired						= std::max(0, b.type.gasPrice() - getFreeGas());

	// get a candidate worker to move to this location
	BWAPI::Unit moveWorker			= WorkerManager::Instance().getMoveWorker(walkToPosition);

	// Conditions under which to move the worker: 
	//		- there's a valid worker to move
	//		- we haven't yet assigned a worker to move to this location
	//		- the build position is valid
	//		- we will have the required resources by the time the worker gets there
	if (moveWorker && haveLocationForThisBuilding && !assignedWorkerForThisBuilding && (predictedTilePosition != BWAPI::TilePositions::None) &&
		WorkerManager::Instance().willHaveResources(mineralsRequired, gasRequired, moveWorker->getDistance(walkToPosition)) )
	{
		// we have assigned a worker
		assignedWorkerForThisBuilding = true;

		// tell the worker manager to move this worker
		WorkerManager::Instance().setMoveWorker(mineralsRequired, gasRequired, walkToPosition);
	}
}

void ProductionManager::performCommand(BWAPI::UnitCommandType t) 
{
	// if it is a cancel construction, it is probably the extractor trick
	if (t == BWAPI::UnitCommandTypes::Cancel_Construction)
	{
		BWAPI::Unit extractor = nullptr;
		for (auto & unit : BWAPI::Broodwar->self()->getUnits())
		{
			if (unit->getType() == BWAPI::UnitTypes::Zerg_Extractor)
			{
				extractor = unit;
			}
		}

		if (extractor)
		{
			extractor->cancelConstruction();
		}
	}
}

int ProductionManager::getFreeMinerals()
{
	return BWAPI::Broodwar->self()->minerals() - BuildingManager::Instance().getReservedMinerals();
}

int ProductionManager::getFreeGas()
{
	return BWAPI::Broodwar->self()->gas() - BuildingManager::Instance().getReservedGas();
}

// return whether or not we meet resources, including building reserves
bool ProductionManager::meetsReservedResources(MetaType type) 
{
	// return whether or not we meet the resources
	return (type.mineralPrice() <= getFreeMinerals()) && (type.gasPrice() <= getFreeGas());
}


// selects a unit of a given type
BWAPI::Unit ProductionManager::selectUnitOfType(BWAPI::UnitType type, BWAPI::Position closestTo) 
{
	// if we have none of the unit type, return nullptr right away
	if (BWAPI::Broodwar->self()->completedUnitCount(type) == 0) 
	{
		return nullptr;
	}

	BWAPI::Unit unit = nullptr;

	// if we are concerned about the position of the unit, that takes priority
    if (closestTo != BWAPI::Positions::None) 
    {
		double minDist(1000000);

		for (auto & u : BWAPI::Broodwar->self()->getUnits()) 
        {
			if (u->getType() == type) 
            {
				double distance = u->getDistance(closestTo);
				if (!unit || distance < minDist) {
					unit = u;
					minDist = distance;
				}
			}
		}

	// if it is a building and we are worried about selecting the unit with the least
	// amount of training time remaining
	} 
    else if (type.isBuilding()) 
    {
		for (auto & u : BWAPI::Broodwar->self()->getUnits()) 
        {
            UAB_ASSERT(u != nullptr, "Unit was null");

			if (u->getType() == type && u->isCompleted() && !u->isTraining() && !u->isLifted() &&u->isPowered()) {

				return u;
			}
		}
		// otherwise just return the first unit we come across
	} 
    else 
    {
		for (auto & u : BWAPI::Broodwar->self()->getUnits()) 
		{
            UAB_ASSERT(u != nullptr, "Unit was null");

			if (u->getType() == type && u->isCompleted() && u->getHitPoints() > 0 && !u->isLifted() &&u->isPowered()) 
			{
				return u;
			}
		}
	}

	// return what we've found so far
	return nullptr;
}

void ProductionManager::populateTypeCharMap()
{
	typeCharMap['p'] = MetaType(BWAPI::UnitTypes::Protoss_Probe);
	typeCharMap['z'] = MetaType(BWAPI::UnitTypes::Protoss_Zealot);
	typeCharMap['d'] = MetaType(BWAPI::UnitTypes::Protoss_Dragoon);
	typeCharMap['t'] = MetaType(BWAPI::UnitTypes::Protoss_Dark_Templar);
	typeCharMap['c'] = MetaType(BWAPI::UnitTypes::Protoss_Corsair);
	typeCharMap['e'] = MetaType(BWAPI::UnitTypes::Protoss_Carrier);
	typeCharMap['h'] = MetaType(BWAPI::UnitTypes::Protoss_High_Templar);
	typeCharMap['n'] = MetaType(BWAPI::UnitTypes::Protoss_Photon_Cannon);
	typeCharMap['a'] = MetaType(BWAPI::UnitTypes::Protoss_Arbiter);
	typeCharMap['r'] = MetaType(BWAPI::UnitTypes::Protoss_Reaver);
	typeCharMap['o'] = MetaType(BWAPI::UnitTypes::Protoss_Observer);
	typeCharMap['s'] = MetaType(BWAPI::UnitTypes::Protoss_Scout);
	typeCharMap['l'] = MetaType(BWAPI::UpgradeTypes::Leg_Enhancements);
	typeCharMap['v'] = MetaType(BWAPI::UpgradeTypes::Singularity_Charge);
}

void ProductionManager::drawProductionInformation(int x, int y)
{
    if (!Config::Debug::DrawProductionInfo)
    {
        return;
    }

	// fill prod with each unit which is under construction
	std::vector<BWAPI::Unit> prod;
	for (auto & unit : BWAPI::Broodwar->self()->getUnits())
	{
        UAB_ASSERT(unit != nullptr, "Unit was null");

		if (unit->isBeingConstructed())
		{
			prod.push_back(unit);
		}
	}
	
	// sort it based on the time it was started
	std::sort(prod.begin(), prod.end(), CompareWhenStarted());

	BWAPI::Broodwar->drawTextScreen(x, y, "\x04 Build Order Info:");
    BWAPI::Broodwar->drawTextScreen(x-30, y+20, "\x04 TIME");
	BWAPI::Broodwar->drawTextScreen(x, y+20, "\x04 UNIT NAME");

	size_t reps = prod.size() < 10 ? prod.size() : 10;

	y += 30;
	int yy = y;

	// for each unit in the queue
	for (auto & unit : prod) 
    {
		std::string prefix = "\x07";

		yy += 10;

		BWAPI::UnitType t = unit->getType();
        if (t == BWAPI::UnitTypes::Zerg_Egg)
        {
            t = unit->getBuildType();
        }

		BWAPI::Broodwar->drawTextScreen(x, yy, " %s%s", prefix.c_str(), t.getName().c_str());
		BWAPI::Broodwar->drawTextScreen(x - 35, yy, "%s%6d", prefix.c_str(), unit->getRemainingBuildTime());
	}

	queue.drawQueueInformation(x, yy+10);
}

ProductionManager & ProductionManager::Instance()
{
	static ProductionManager instance;
	return instance;
}

void ProductionManager::onGameEnd()
{

}

void ProductionManager::queueGasSteal()
{
    queue.queueAsHighestPriority(MetaType(BWAPI::Broodwar->self()->getRace().getRefinery()), true, true);
}