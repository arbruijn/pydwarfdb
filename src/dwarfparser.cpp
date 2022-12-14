#include "dwarfparser.h"

#include <cassert>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <typeinfo>
#include <unistd.h>

#include "dwarfexception.h"
#include "helpers.h"
#include "libdwarfparser.h"
#include "symbolmanager.h"


DwarfParser::Srcfilesdata::Srcfilesdata()
	:
	srcfiles(0),
	srcfilescount(0),
	srcfilesres(DW_DLV_ERROR) {}

DwarfParser::Srcfilesdata::~Srcfilesdata() {
	// TODO: these are rogue deallocations!
	Dwarf_Debug dbg = 0;
	for (Dwarf_Signed sri = 0; sri < this->srcfilescount; ++sri) {
		dwarf_dealloc(dbg, this->srcfiles[sri], DW_DLA_STRING);
	}
	dwarf_dealloc(dbg, this->srcfiles, DW_DLA_LIST);
	this->srcfilesres = DW_DLV_ERROR;
	this->srcfiles = 0;
	this->srcfilescount = 0;
}



DwarfParser::DwarfParser(int fd, SymbolManager *manager)
	:
	dbg(),
	fd(fd),
	is_fd_owner(true),
	res(DW_DLV_ERROR),
	error(),
	errhand(),
	errarg(),
	curCUOffset(0),
	nextCUOffset(0),
	manager{manager} {

	static uint32_t nextFileID = 0;
	static std::mutex nextFileMutex;
	std::lock_guard<std::mutex> lock(nextFileMutex);
	this->fileID = ++nextFileID;

	//std::cout << "Loaded parser with id: " << fileID << std::endl;
	res = dwarf_init(this->fd, DW_DLC_READ, errhand, errarg, &dbg, &error);
	if (res != DW_DLV_OK) {
		throw DwarfException(dwarf_errmsg(error));
	}
}

DwarfParser::~DwarfParser(){
	// Done reading DWARF symbols
	res = dwarf_finish(dbg,&error);
	if(res != DW_DLV_OK) {
		printf("dwarf_finish failed: %s\n", dwarf_errmsg(error));
	}

	if (this->is_fd_owner) {
		close(fd);
	}
}

void DwarfParser::parseDwarfFromFilename(const std::string &filename,
                                         SymbolManager *mgr) {
	int fd = open(filename.c_str(), O_RDONLY);
	DwarfParser::parseDwarfFromFD(fd, mgr);
}

void DwarfParser::parseDwarfFromFD(int fd, SymbolManager *mgr) {
	// will close the fd when destructed.
	DwarfParser parser{fd, mgr};
	parser.read_cu_list();
}

uint32_t DwarfParser::getFileID() {
	return this->fileID;
}

void DwarfParser::read_cu_list() {
	Dwarf_Unsigned cu_header_length = 0;
	Dwarf_Half version_stamp        = 0;
	Dwarf_Unsigned abbrev_offset    = 0;
	Dwarf_Half address_size         = 0;
	Dwarf_Unsigned next_cu_header   = 0;
	Dwarf_Error error;

	while (true) {
		this->curCUOffset = this->nextCUOffset;
		Srcfilesdata sf;
		Dwarf_Die no_die = 0;
		Dwarf_Die cu_die = 0;
		int res          = DW_DLV_ERROR;

		res = dwarf_next_cu_header(
			dbg,
			&cu_header_length,
			&version_stamp,
			&abbrev_offset,
			&address_size,
			&next_cu_header,
			&error
		);

		if (res == DW_DLV_ERROR) {
			printf("Error in dwarf_next_cu_header\n");
			exit(1);
		}

		if (res == DW_DLV_NO_ENTRY) {
			/* Done. */
			break;
		}

		this->nextCUOffset = next_cu_header;
		//std::cout << std::hex <<
		//"cu_header_length " <<  cu_header_length <<
		//"\n version_stamp " << version_stamp <<
		//"\n abbrev_offset " << abbrev_offset <<
		//"\n address_size " << address_size <<
		//"\n next_cu_header " << next_cu_header <<
		//std::dec << std::endl;

		/* The CU will have a single sibling, a cu_die. */
		res = dwarf_siblingof(dbg, no_die, &cu_die, &error);
		if (res == DW_DLV_ERROR) {
			printf("Error in dwarf_siblingof on CU die \n");
			exit(1);
		}
		if (res == DW_DLV_NO_ENTRY) {
			/* Impossible case. */
			printf("no entry! in dwarf_siblingof on CU die \n");
			exit(1);
		}
		this->get_die_and_siblings(cu_die, nullptr, 0, sf);
		dwarf_dealloc(dbg, cu_die, DW_DLA_DIE);
	}
}

void DwarfParser::get_die_and_siblings(const Dwarf_Die &in_die,
                                       Symbol *parent,
                                       int in_level,
                                       Srcfilesdata sf) {
	int res           = DW_DLV_ERROR;
	Dwarf_Die cur_die = in_die;
	Dwarf_Die child   = 0;
	Dwarf_Error error;

	Symbol *cursym = nullptr;

	cursym = this->initSymbolFromDie(in_die, parent, in_level, sf);

	for (;;) {
		Dwarf_Die sib_die = 0;
		res = dwarf_child(cur_die, &child, &error);
		if (res == DW_DLV_ERROR) {
			printf("Error in dwarf_child , level %d \n", in_level);
			exit(1);
		}
		if (res == DW_DLV_OK) {
			get_die_and_siblings(child, cursym, in_level + 1, sf);
		}
		/* res == DW_DLV_NO_ENTRY */
		res = dwarf_siblingof(dbg, cur_die, &sib_die, &error);
		if (res == DW_DLV_ERROR) {
			printf("Error in dwarf_siblingof , level %d \n", in_level);
			exit(1);
		}
		if (res == DW_DLV_NO_ENTRY) {
			/* Done at this level. */
			break;
		}
		/* res == DW_DLV_OK */
		if (cur_die != in_die) {
			dwarf_dealloc(dbg, cur_die, DW_DLA_DIE);
		}
		cur_die = sib_die;
		cursym  = this->initSymbolFromDie(cur_die, parent, in_level, sf);
	}
	return;
}

void DwarfParser::print_die_data(const Dwarf_Die &print_me,
                                 int level,
                                 Srcfilesdata sf) {
	UNUSED(sf);
	char *name          = nullptr;
	Dwarf_Half tag      = 0;
	const char *tagname = nullptr;
	int localname       = 0;

	int res = dwarf_diename(print_me, &name, &error);

	std::cout << this->getDieName(print_me) << " @ " << std::hex
	          << this->getDieOffset(print_me) << std::dec << std::endl;

	if (res == DW_DLV_ERROR) {
		printf("Error in dwarf_diename , level %d \n", level);
		exit(1);
	}
	if (res == DW_DLV_NO_ENTRY) {
		name      = (char *)"<no DW_AT_name attr>";
		localname = 1;
	}
	res = dwarf_tag(print_me, &tag, &error);
	if (res != DW_DLV_OK) {
		printf("Error in dwarf_tag , level %d \n", level);
		exit(1);
	}
	res = dwarf_get_TAG_name(tag, &tagname);
	if (res != DW_DLV_OK) {
		printf("Error in dwarf_get_TAG_name , level %d \n", level);
		exit(1);
	}
	printf("--\nDescription of tag: %s\n", tagname);

	Dwarf_Attribute *attrbuf = nullptr;
	Dwarf_Signed attrcount   = 0;
	res = dwarf_attrlist(print_me, &attrbuf, &attrcount, &error);
	if (res == DW_DLV_ERROR) {
		throw DwarfException("Unable to get attribute list");
	} else if (res != DW_DLV_NO_ENTRY) {
		for (Dwarf_Signed i = 0; i < attrcount; ++i) {
			Dwarf_Half aform;
			res = dwarf_whatattr(attrbuf[i], &aform, &error);
			if (res == DW_DLV_OK) {
				const char *atname;
				dwarf_get_AT_name(aform, &atname);

				Dwarf_Attribute myattr;
				res = dwarf_attr(print_me, aform, &myattr, &error);
				if (res == DW_DLV_ERROR) {
					throw DwarfException("Error in dwarf_attr\n");
				}
				uint16_t formid = ((uint16_t *)myattr)[1];

				uint64_t number;
				std::string name;
				const char *formname;
				dwarf_get_FORM_name(formid, &formname);
				std::cout << atname << " with type: " << formname << ": ";

				switch (formid) {
				case DW_FORM_addr:
					number = this->getDieAttributeAddress(print_me, aform);
					std::cout << number;
					break;
				case DW_FORM_data1:
				case DW_FORM_data2:
				case DW_FORM_data4:
				case DW_FORM_data8:
					number = this->getDieAttributeNumber(print_me, aform);
					std::cout << number;
					break;
				case DW_FORM_ref4:
				case DW_FORM_sdata:
					number = this->getDieAttributeNumber(print_me, aform);
					std::cout << std::hex << (int64_t)number << std::dec;
					break;
				case DW_FORM_exprloc: std::cout << "<unavailable> - "; break;
				case DW_FORM_flag_present:
					number = this->getDieAttributeFlag(print_me, aform);
					std::cout << ((number) ? "True" : "False");
					break;
				case DW_FORM_strp:
					name = this->getDieAttributeString(print_me, aform);
					std::cout << name;
					break;
				default:
					dwarf_get_FORM_name(formid, &formname);
					std::cout << "<unavailable> - " << formname;
					break;
				}

				std::cout << std::endl;
			}
			dwarf_dealloc(dbg, attrbuf[i], DW_DLA_ATTR);
		}
		dwarf_dealloc(dbg, attrbuf, DW_DLA_LIST);
	}

	printf("<%d> tag: %d %s  name: \"%s\"\n", level, tag, tagname, name);

	if (!localname) {
		dwarf_dealloc(dbg, name, DW_DLA_STRING);
	}
}

template <class T>
T *DwarfParser::getRefTypeInstance(const Dwarf_Die &object,
                                   const std::string &dieName) {
	T *cursym;

	RefBaseType *rbt = this->manager->findRefBaseTypeByName(dieName);
	cursym = dynamic_cast<T *>(rbt);
	if (!rbt) {
		return new T(this->manager, this, object, dieName);
	} else if (!cursym) {
		std::cout << "RefBaseType with same name but different type!"
		          << typeid(T).name() << " vs " << typeid(rbt).name() << " "
		          << rbt->getName() << std::endl;
		std::cout << "Previous id: " << std::hex << rbt->getID()
		          << " current ID: " << this->getDieOffset(object) << std::dec
		          << std::endl;
		return nullptr;
	}
	cursym->addAlternativeDwarfID(this->getDieOffset(object), fileID);
	return cursym;
}

template <>
Function *DwarfParser::getTypeInstance(const Dwarf_Die &object,
                                       const std::string &dieName) {
	Function *cursym;

	cursym = this->manager->findBaseTypeByName<Function>(dieName);
	if (!cursym) {
		return new Function(this->manager, this, object, dieName);
	}
	cursym->update(this, object);
	cursym->addAlternativeDwarfID(this->getDieOffset(object), fileID);
	return cursym;
}

template <>
Variable *DwarfParser::getTypeInstance(const Dwarf_Die &object,
                                       const std::string &dieName) {
	Variable *cursym;

	cursym = this->manager->findVariableByName(dieName);
	if (!cursym) {
		return new Variable(this->manager, this, object, dieName);
	}
	cursym->update(this, object);
	cursym->addAlternativeDwarfID(this->getDieOffset(object), fileID);
	return cursym;
}

template <class T>
T *DwarfParser::getTypeInstance(const Dwarf_Die &object,
                                const std::string &dieName) {
	T *cursym = this->manager->findBaseTypeByName<T>(dieName);
	if (!cursym) {
		return new T(this->manager, this, object, dieName);
	}
	// TODO Include
	// We need to update the representation of the object that we
	// already know about
	// cursym->update(object);
	cursym->addAlternativeDwarfID(this->getDieOffset(object), fileID);
	return cursym;
}

Symbol *DwarfParser::initSymbolFromDie(const Dwarf_Die &cur_die,
                                       Symbol *parent,
                                       int level,
                                       Srcfilesdata sf) {
	Dwarf_Half tag      = 0;
	const char *tagname = nullptr;
	res = dwarf_tag(cur_die, &tag, &error);
	if (res != DW_DLV_OK) {
		throw DwarfException("Error in dwarf_get_TAG_name");
	}

	std::string name = this->getDieName(cur_die);

	Symbol *cursym = nullptr;

	Structured *structured = nullptr;
	Enum *enumType         = nullptr;
	Array *array           = nullptr;
	Function *function     = nullptr;

	switch (tag) {
	case DW_TAG_typedef:
		cursym = this->getRefTypeInstance<Typedef>(cur_die, name);
		break;
	case DW_TAG_structure_type:
	case DW_TAG_class_type:
		if (this->dieHasAttr(cur_die, DW_AT_declaration))
			break;
		cursym = this->getTypeInstance<Struct>(cur_die, name);
		break;
	case DW_TAG_union_type:
		cursym = this->getTypeInstance<Union>(cur_die, name);
		break;
	case DW_TAG_member:
		if (!parent) {
			std::cout << "Parent not set" << std::endl;
			print_die_data(cur_die,level,sf);
			break;
		}
		structured = dynamic_cast<Structured *>(parent);
		if (structured) {
			cursym = structured->addMember(this->manager, this,
			                               cur_die, name);
			// cursym = this->getTypeInstance<Variable>(cur_die, name);
			break;
		} else {
			// print_die_data(cur_die,level,sf);
			// class also contains members
			// throw DwarfException("Parent structured not set");
		}
		break;
	case DW_TAG_base_type:
		cursym = this->getTypeInstance<BaseType>(cur_die, name);
		break;
	case DW_TAG_pointer_type:
		cursym = this->getRefTypeInstance<Pointer>(cur_die, name);
		break;
	case DW_TAG_const_type:
		cursym = this->getRefTypeInstance<ConstType>(cur_die, name);
		break;
	case DW_TAG_enumeration_type:
		cursym = this->getTypeInstance<Enum>(cur_die, name);
		break;
	case DW_TAG_enumerator:
		assert(parent);
		enumType = dynamic_cast<Enum *>(parent);
		if (enumType) {
			enumType->addEnum(this->manager, this, cur_die, name);
		} else {
			print_die_data(cur_die, level, sf);
		}
		break;
	case DW_TAG_variable:
		if (this->dieHasAttr(cur_die, DW_AT_specification)) {
			if (this->dieHasAttr(cur_die, DW_AT_location)) {
				// This is an initializer for another object
				uint64_t ref = this->getDieAttributeNumber(cur_die, DW_AT_specification);
				Symbol *s = this->manager->findSymbolByID(
					this->manager->getID(ref, this->getFileID()));
				Variable *v = s ? dynamic_cast<Variable *>(s) : NULL;
				if (v)
					v->update(this, cur_die);
			}
			break;
		}
		cursym = this->getTypeInstance<Variable>(cur_die, name);
		break;
	case DW_TAG_array_type:
		cursym = new Array(this->manager, this, cur_die, name);
		break;
	case DW_TAG_subrange_type:
		assert(parent);
		array = dynamic_cast<Array *>(parent);
		if (array) {
			array->update(this, cur_die);
		} else {
			print_die_data(cur_die, level, sf);
		}
		break;
	case DW_TAG_subprogram:
		if (this->dieHasAttr(cur_die, DW_AT_specification)) {
			if (this->dieHasAttr(cur_die, DW_AT_low_pc)) {
				// This is an initializer for another object
				uint64_t ref = this->getDieAttributeNumber(cur_die, DW_AT_specification);
				Symbol *s = this->manager->findSymbolByID(
					this->manager->getID(ref, this->getFileID()));
				Function *f = s ? dynamic_cast<Function *>(s) : NULL;
				if (f)
					f->update(this, cur_die);
			}
			break;
		}

		// case DW_TAG_subroutine_type:
		cursym = this->getTypeInstance<Function>(cur_die, name);
		// cursym = new Function(cur_die);
		// print_die_data(cur_die, level, sf);
		break;
	case DW_TAG_formal_parameter:
		function = dynamic_cast<Function *>(parent);
		if (function) {
			function->addParam(this, cur_die);
		}
		break;
	case DW_TAG_compile_unit:
	case DW_TAG_namespace:
	case DW_TAG_imported_declaration:
	//case DW_TAG_class_type:
	case DW_TAG_lexical_block:
		/* This tag is currently not supported */
		// TODO: warning message
		break;

	default:
		res = dwarf_get_TAG_name(tag, &tagname);
		if (res != DW_DLV_OK) {
			throw DwarfException("Error in dwarf_get_TAG_name");
		}
		// printf("We are currently not interested in the tag: %s\n", tagname);
		// print_die_data(cur_die,level,sf);
	}
	return cursym;
}

std::string DwarfParser::getDieName(const Dwarf_Die &die) {
	char *name = nullptr;

	std::string result = "";

	int res = dwarf_diename(die, &name, &error);

	if (res == DW_DLV_ERROR) {
		throw DwarfException("Error in dwarf_diename\n");
	}
	if (name != nullptr) {
		result = std::string(name);
	}
	dwarf_dealloc(dbg, name, DW_DLA_STRING);
	return result;
}

uint64_t DwarfParser::getDieOffset(const Dwarf_Die &die) {
	Dwarf_Off offset;

	int res = dwarf_dieoffset(die, &offset, &error);

	if (res == DW_DLV_ERROR) {
		throw DwarfException("Error in dwarf_dieoffset\n");
	}
	return (uint64_t)offset;
}

uint64_t DwarfParser::getDieByteSize(const Dwarf_Die &die) {
	Dwarf_Unsigned size;

	int res = dwarf_bytesize(die, &size, &error);

	if (res == DW_DLV_ERROR) {
		throw DwarfException("Error in dwarf_bytesize\n");
	}
	return (uint64_t)size;
}

uint64_t DwarfParser::getDieBitOffset(const Dwarf_Die &die) {
	Dwarf_Unsigned size;

	int res = dwarf_bitoffset(die, &size, &error);

	if (res == DW_DLV_ERROR) {
		throw DwarfException("Error in dwarf_bitoffset\n");
	}
	return (uint64_t)size;
}

bool DwarfParser::dieHasAttr(const Dwarf_Die &die, const Dwarf_Half &attr) {
	Dwarf_Bool hasattr;
	dwarf_hasattr(die, attr, &hasattr, &error);
	if (hasattr == 0) {
		return false;
	}
	return true;
}

uint64_t parseBlock(uint64_t blen, uint8_t *bdata) {
	uint64_t result = 0;
	switch (bdata[0]) {
	case DW_OP_addr:
		if (blen > 10)
			std::cout << "Error with " << std::hex << " Die: "
			          // << this->getDieOffset(die) << std::dec
			          << " Block length mismatch" << std::endl;
		// assert(block->bl_len <= 10);
		for (Dwarf_Unsigned i = 1; i < 9; i++) {
			result = result << 8;
			result += bdata[9 - i];
			// TODO Handle the last byte DW_OP_stack_value
		}
		break;
	case DW_OP_plus_uconst:
		// For further details see: binutils/dwarf.c:256
		result = 0;
		assert(blen <= 9);
		for (Dwarf_Unsigned i = 1; i < blen; i++) {
			uint8_t byte = bdata[i];
			result |= ((uint64_t)(byte & 0x7f)) << (i - 1) * 7;
			if ((byte & 0x80) == 0)
				break;
		}
		break;
	default:
		result = 0;
		break;
		// TODO add other operators
		// const char * atname;
		// dwarf_get_AT_name(attr, &atname);
		// const char* formname;
		// dwarf_get_FORM_name(formid, &formname);

		// std::cout << std::hex << getDieOffset(die) << std::dec << std::endl;
		// std::cout << atname << ": ";
		// std::cout << formname << std::endl;
		// std::cout << "Value: " << std::endl;
		// for(Dwarf_Unsigned i = 0; i < block->bl_len; i++){
		//	std::cout << std::hex <<
		//		(uint32_t) ((uint8_t*) block->bl_data)[i] <<
		//		std::dec << " ";
		//}
		// std::cout << std::endl;
		// result = 0;
	}
	return result;
}

uint64_t DwarfParser::getDieAttributeNumber(const Dwarf_Die &die,
                                            const Dwarf_Half &attr) {
	uint64_t result;
	uint64_t result2;
	Dwarf_Attribute myattr;
	Dwarf_Bool hasattr;
	Dwarf_Block *block;

	int res = dwarf_hasattr(die, attr, &hasattr, &error);
	if (hasattr == 0) {
		throw DwarfException("Attr not in Die\n");
	}

	res = dwarf_attr(die, attr, &myattr, &error);
	if (res == DW_DLV_ERROR) {
		throw DwarfException("Error in dwarf_attr\n");
	}

	uint16_t formid = ((uint16_t *)myattr)[1];
	switch (formid) {
	case DW_FORM_block1:
	case DW_FORM_block2:
	case DW_FORM_block4:
		res = dwarf_formblock(myattr, &block, &error);
		if (res == DW_DLV_OK) {
			assert(block->bl_len > 0);
			result = parseBlock(block->bl_len, (uint8_t *)block->bl_data);
			dwarf_dealloc(dbg, block, DW_DLA_BLOCK);
			return result;
		}
		break;
	case DW_FORM_data1:
	case DW_FORM_data2:
	case DW_FORM_data4:
	case DW_FORM_data8:
		res = dwarf_formudata(myattr, (Dwarf_Unsigned *)&result, &error);
		if (res == DW_DLV_OK) {
			return result;
		}
		break;
	case DW_FORM_ref4:
		res = dwarf_formref(myattr, (Dwarf_Off *)&result, &error);
		if (res == DW_DLV_OK) {
			return result + this->curCUOffset;
		}
		break;
	case DW_FORM_sdata:
		res = dwarf_formsdata(myattr, (Dwarf_Signed *)&result, &error);
		if (res == DW_DLV_OK) {
			return result;
		}
		break;
	case DW_FORM_addr:
		res = dwarf_formaddr(myattr, (Dwarf_Addr *)&result, &error);
		if (res == DW_DLV_OK) {
			return result;
		}
		break;
	case DW_FORM_sec_offset:
		res = dwarf_global_formref(myattr, (Dwarf_Off *)&result, &error);
		// TODO we do not know where the offset is relative to...
		if (res == DW_DLV_OK) {
			return result + this->curCUOffset;
		}
		break;
	case DW_FORM_exprloc:
		res = dwarf_formexprloc(
		    myattr, (Dwarf_Unsigned *)&result, (Dwarf_Ptr *)&result2, &error);
		if (res == DW_DLV_OK) {
			return parseBlock(result, (uint8_t *)result2);
		}
		break;
	default:
		const char *formname;
		dwarf_get_FORM_name(formid, &formname);
		std::cout << formname << " currently not supported" << std::endl;
	}

	const char *atname;
	dwarf_get_AT_name(attr, &atname);
	const char *formname;
	dwarf_get_FORM_name(formid, &formname);

	std::cout << this->getDieName(die) << std::endl;
	std::cout << atname << ": ";
	std::cout << formname << std::endl;

	throw DwarfException("Error in getDieAttributeNumber\n");
	return 0;
}

std::string DwarfParser::getDieAttributeString(const Dwarf_Die &die,
                                               const Dwarf_Half &attr) {
	char *str;
	std::string result;
	Dwarf_Attribute myattr;
	Dwarf_Bool hasattr;

	int res = dwarf_hasattr(die, attr, &hasattr, &error);
	if (hasattr == 0) {
		throw DwarfException("Attr not in Die\n");
	}

	res = dwarf_attr(die, attr, &myattr, &error);
	if (res == DW_DLV_ERROR) {
		throw DwarfException("Error in dwarf_attr\n");
	}
	res = dwarf_formstring(myattr, &str, &error);
	if (res == DW_DLV_OK) {
		result = std::string(str);
		dwarf_dealloc(dbg, str, DW_DLA_STRING);
		return result;
	}

	throw DwarfException("Error in getDieAttributeString\n");
	return nullptr;
}

uint64_t DwarfParser::getDieAttributeAddress(const Dwarf_Die &die,
                                             const Dwarf_Half &attr) {
	uint64_t result;
	Dwarf_Attribute myattr;
	Dwarf_Bool hasattr;

	int res = dwarf_hasattr(die, attr, &hasattr, &error);
	if (hasattr == 0) {
		throw DwarfException("Attr not in Die\n");
	}

	res = dwarf_attr(die, attr, &myattr, &error);
	if (res == DW_DLV_ERROR) {
		throw DwarfException("Error in dwarf_attr\n");
	}

	res = dwarf_formaddr(myattr, (Dwarf_Addr *)&result, &error);
	if (res == DW_DLV_OK) {
		return result;
	}

	throw DwarfException("Error in getDieAttributeAddress\n");
	return 0;
}

bool DwarfParser::isDieExternal(const Dwarf_Die &die) {
	return this->getDieAttributeFlag(die, DW_AT_external);
}

bool DwarfParser::isDieDeclaration(const Dwarf_Die &die) {
	return this->getDieAttributeFlag(die, DW_AT_declaration);
}

bool DwarfParser::getDieAttributeFlag(const Dwarf_Die &die,
                                      const Dwarf_Half &attr) {
	Dwarf_Attribute myattr;
	Dwarf_Bool hasattr;

	int res = dwarf_hasattr(die, attr, &hasattr, &error);
	if (hasattr == 0) {
		return false;
		throw DwarfException("Attr not in Die\n");
	}

	res = dwarf_attr(die, attr, &myattr, &error);
	if (res == DW_DLV_ERROR) {
		throw DwarfException("Error in dwarf_attr\n");
	}

	res = dwarf_formflag(myattr, &hasattr, &error);
	if (res == DW_DLV_OK) {
		return hasattr;
	}

	throw DwarfException("Error in getDieAttributeAddress\n");
	return 0;
}
