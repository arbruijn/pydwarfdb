#include "variable.h"

#include <cassert>

#include "helpers.h"
#include <iostream>

#include "instance.h"
#include "symbolmanager.h"

Variable::Variable(SymbolManager *mgr,
                   DwarfParser *parser,
                   const Dwarf_Die &object,
                   const std::string &name)
	:
	Symbol{mgr, parser, object, name},
	ReferencingType{mgr, parser, object},
	location{0} {

	this->Symbol::manager->addVariable(this);
	if (parser->dieHasAttr(object, DW_AT_location)) {
		this->location = parser->getDieAttributeNumber(object, DW_AT_location);
	}
}

Variable::~Variable() {}

uint64_t Variable::getLocation() {
	return this->location;
}

void Variable::update(DwarfParser *parser, const Dwarf_Die &object) {
	if (this->location != 0)
		return;
	if (parser->dieHasAttr(object, DW_AT_location)) {
		this->location = parser->getDieAttributeNumber(object, DW_AT_location);
	}
}

void Variable::setLocation(uint64_t location) {
	this->location = location;
}

Instance Variable::getInstance() {
	assert(this->location);
	Instance instance = Instance(this->getBaseType(),
	                             this->location);
	return instance;
}

void Variable::print() const {
	std::cout << "Variable:" << std::endl;
	std::cout << "\t Location:     " << std::hex
	          << "0x" << this->location << std::dec << std::endl;
	Symbol::print();
	ReferencingType::print();
}
