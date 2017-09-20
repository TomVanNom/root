// Author: Enrico Guiraud, Danilo Piparo CERN  03/2017

/*************************************************************************
 * Copyright (C) 1995-2016, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#include "TClass.h"
#include "TRegexp.h"

#include "ROOT/TDFInterface.hxx"

#include <vector>
#include <string>
using namespace ROOT::Experimental::TDF;
using namespace ROOT::Internal::TDF;
using namespace ROOT::Detail::TDF;

namespace ROOT {
namespace Experimental {
namespace TDF {
// extern templates
template class TInterface<TLoopManager>;
template class TInterface<TFilterBase>;
}
}

namespace Internal {
namespace TDF {
// Match expression against names of branches passed as parameter
// Return vector of names of the branches used in the expression
std::vector<std::string> FindUsedColumnNames(std::string_view expression, TObjArray *branches,
                                             const ColumnNames_t &customColumns, const ColumnNames_t &dsColumns)
{
   // To help matching the regex
   const std::string paddedExpr = " " + std::string(expression) + " ";
   int paddedExprLen = paddedExpr.size();
   static const std::string regexBit("[^a-zA-Z0-9_]");

   std::vector<std::string> usedBranches;

   // Check which custom columns match
   for (auto brName : customColumns) {
      std::string bNameRegexContent = regexBit + brName + regexBit;
      TRegexp bNameRegex(bNameRegexContent.c_str());
      if (-1 != bNameRegex.Index(paddedExpr.c_str(), &paddedExprLen)) {
         usedBranches.emplace_back(brName);
      }
   }

   // Check which tree branches match
   if (branches) {
      for (auto bro : *branches) {
         auto brName = bro->GetName();
         std::string bNameRegexContent = regexBit + brName + regexBit;
         TRegexp bNameRegex(bNameRegexContent.c_str());
         if (-1 != bNameRegex.Index(paddedExpr.c_str(), &paddedExprLen)) {
            usedBranches.emplace_back(brName);
         }
      }
   }

   // Check which data-source columns match
   for (auto col : dsColumns) {
      std::string bNameRegexContent = regexBit + col + regexBit;
      TRegexp bNameRegex(bNameRegexContent.c_str());
      if (-1 != bNameRegex.Index(paddedExpr.c_str(), &paddedExprLen)) {
         // if not already found among the custom columns
         if (std::find(usedBranches.begin(), usedBranches.end(), col) == usedBranches.end())
            usedBranches.emplace_back(col);
      }
   }

   return usedBranches;
}

// Jit a string filter or a string temporary column, call this->Define or this->Filter as needed
// Return pointer to the new functional chain node returned by the call, cast to Long_t
Long_t JitTransformation(void *thisPtr, std::string_view methodName, std::string_view interfaceTypeName,
                         std::string_view name, std::string_view expression, TObjArray *branches,
                         const std::vector<std::string> &customColumns,
                         const std::map<std::string, TmpBranchBasePtr_t> &tmpBookedBranches, TTree *tree,
                         std::string_view returnTypeName, TDataSource *ds)
{
   const auto &dsColumns = ds ? ds->GetColumnNames() : ColumnNames_t{};
   auto usedBranches = FindUsedColumnNames(expression, branches, customColumns, dsColumns);
   auto exprNeedsVariables = !usedBranches.empty();

   // Move to the preparation of the jitting
   // We put all of the jitted entities in a namespace called
   // __tdf_filter_N, where N is a monotonically increasing index.
   std::vector<std::string> usedBranchesTypes;
   std::stringstream ss;
   static unsigned int iNs = 0U;
   ss << "__tdf_" << iNs++;
   const auto nsName = ss.str();
   ss.str("");

   if (exprNeedsVariables) {
      // Declare a namespace and inside it the variables in the expression
      ss << "namespace " << nsName;
      ss << " {\n";
      for (auto brName : usedBranches) {
         // The map is a const reference, so no operator[]
         auto tmpBrIt = tmpBookedBranches.find(brName);
         auto tmpBr = tmpBrIt == tmpBookedBranches.end() ? nullptr : tmpBrIt->second.get();
         auto brTypeName = ColumnName2ColumnTypeName(brName, tree, tmpBr, ds);
         ss << brTypeName << " " << brName << ";\n";
         usedBranchesTypes.emplace_back(brTypeName);
      }
      ss << "}";
      auto variableDeclarations = ss.str();
      ss.str("");
      // We need ProcessLine to trigger auto{parsing,loading} where needed
      TInterpreter::EErrorCode interpErrCode;
      gInterpreter->ProcessLine(variableDeclarations.c_str(), &interpErrCode);
      if (TInterpreter::EErrorCode::kNoError != interpErrCode) {
         std::string msg = "Cannot declare these variables:  ";
         msg += variableDeclarations;
         msg += "\nInterpreter error code is " + std::to_string(interpErrCode) + ".";
         throw std::runtime_error(msg);
      }
   }

   // Declare within the same namespace, the expression to make sure it
   // is proper C++
   ss << "namespace " << nsName << "{ auto res = " << expression << ";}\n";
   // Headers must have been parsed and libraries loaded: we can use Declare
   if (!gInterpreter->Declare(ss.str().c_str())) {
      std::string msg = "Cannot interpret this expression: ";
      msg += " ";
      msg += ss.str();
      throw std::runtime_error(msg);
   }

   // Now we build the lambda and we invoke the method with it in the jitted world
   ss.str("");
   ss << "[](";
   for (unsigned int i = 0; i < usedBranchesTypes.size(); ++i) {
      // We pass by reference to avoid expensive copies
      ss << usedBranchesTypes[i] << "& " << usedBranches[i] << ", ";
   }
   if (!usedBranchesTypes.empty())
      ss.seekp(-2, ss.cur);
   ss << "){ return " << expression << ";}";
   auto filterLambda = ss.str();

   // The TInterface type to convert the result to. For example, Filter returns a TInterface<TFilter<F,P>> but when
   // returning it from a jitted call we need to convert it to TInterface<TFilterBase> as we are missing information
   // on types F and P at compile time.
   const auto targetTypeName = "ROOT::Experimental::TDF::TInterface<" + std::string(returnTypeName) + ">";

   // Here we have two cases: filter and column
   ss.str("");
   ss << targetTypeName << "(((" << interfaceTypeName << "*)" << thisPtr << ")->" << methodName << "(";
   if (methodName == "Define") {
      ss << "\"" << name << "\", ";
   }
   ss << filterLambda << ", {";
   for (auto brName : usedBranches) {
      ss << "\"" << brName << "\", ";
   }
   if (exprNeedsVariables)
      ss.seekp(-2, ss.cur); // remove the last ",
   ss << "}";

   if (methodName == "Filter") {
      ss << ", \"" << name << "\"";
   }

   ss << "));";

   TInterpreter::EErrorCode interpErrCode;
   auto retVal = gInterpreter->Calc(ss.str().c_str(), &interpErrCode);
   if (TInterpreter::EErrorCode::kNoError != interpErrCode || !retVal) {
      std::string msg = "Cannot interpret the invocation to " + std::string(methodName) + ":  ";
      msg += ss.str();
      if (TInterpreter::EErrorCode::kNoError != interpErrCode) {
         msg += "\nInterpreter error code is " + std::to_string(interpErrCode) + ".";
      }
      throw std::runtime_error(msg);
   }
   return retVal;
}

// Jit and call something equivalent to "this->BuildAndBook<BranchTypes...>(params...)"
// (see comments in the body for actual jitted code)
std::string JitBuildAndBook(const ColumnNames_t &bl, const std::string &prevNodeTypename, void *prevNode,
                            const std::type_info &art, const std::type_info &at, const void *rOnHeap, TTree *tree,
                            const unsigned int nSlots, const std::map<std::string, TmpBranchBasePtr_t> &customColumns,
                            TDataSource *ds)
{
   auto nBranches = bl.size();

   // retrieve pointers to temporary columns (null if the column is not temporary)
   std::vector<TCustomColumnBase *> tmpBranchPtrs(nBranches, nullptr);
   for (auto i = 0u; i < nBranches; ++i) {
      auto tmpBranchIt = customColumns.find(bl[i]);
      if (tmpBranchIt != customColumns.end())
         tmpBranchPtrs[i] = tmpBranchIt->second.get();
   }

   // retrieve branch type names as strings
   std::vector<std::string> columnTypeNames(nBranches);
   for (auto i = 0u; i < nBranches; ++i) {
      const auto columnTypeName = ColumnName2ColumnTypeName(bl[i], tree, tmpBranchPtrs[i], ds);
      if (columnTypeName.empty()) {
         std::string exceptionText = "The type of column ";
         exceptionText += bl[i];
         exceptionText += " could not be guessed. Please specify one.";
         throw std::runtime_error(exceptionText.c_str());
      }
      columnTypeNames[i] = columnTypeName;
   }

   // retrieve type of result of the action as a string
   auto actionResultTypeClass = TClass::GetClass(art);
   if (!actionResultTypeClass) {
      std::string exceptionText = "An error occurred while inferring the result type of an operation.";
      throw std::runtime_error(exceptionText.c_str());
   }
   const auto actionResultTypeName = actionResultTypeClass->GetName();

   // retrieve type of action as a string
   auto actionTypeClass = TClass::GetClass(at);
   if (!actionTypeClass) {
      std::string exceptionText = "An error occurred while inferring the action type of the operation.";
      throw std::runtime_error(exceptionText.c_str());
   }
   const auto actionTypeName = actionTypeClass->GetName();

   // createAction_str will contain the following:
   // ROOT::Internal::TDF::CallBuildAndBook<actionType, branchType1, branchType2...>(
   //   *reinterpret_cast<PrevNodeType*>(prevNode), { bl[0], bl[1], ... }, reinterpret_cast<actionResultType*>(rOnHeap))
   std::stringstream createAction_str;
   createAction_str << "ROOT::Internal::TDF::CallBuildAndBook"
                    << "<" << actionTypeName;
   for (auto &colType : columnTypeNames) createAction_str << ", " << colType;
   createAction_str << ">(*reinterpret_cast<" << prevNodeTypename << "*>(" << prevNode << "), {";
   for (auto i = 0u; i < bl.size(); ++i) {
      if (i != 0u)
         createAction_str << ", ";
      createAction_str << '"' << bl[i] << '"';
   }
   createAction_str << "}, " << nSlots << ", reinterpret_cast<" << actionResultTypeName << "*>(" << rOnHeap << "));";
   return createAction_str.str();
}

bool AtLeastOneEmptyString(const std::vector<std::string_view> strings)
{
   for (const auto &s : strings) {
      if (s.empty())
         return true;
   }
   return false;
}

std::shared_ptr<TFilterBase> UpcastNode(const std::shared_ptr<TFilterBase> ptr)
{
   return ptr;
}

std::shared_ptr<TCustomColumnBase> UpcastNode(const std::shared_ptr<TCustomColumnBase> ptr)
{
   return ptr;
}

std::shared_ptr<TRangeBase> UpcastNode(const std::shared_ptr<TRangeBase> ptr)
{
   return ptr;
}

std::shared_ptr<TLoopManager> UpcastNode(const std::shared_ptr<TLoopManager> ptr)
{
   return ptr;
}

/// Given the desired number of columns and the user-provided list of columns:
/// * fallback to using the first nColumns default columns if needed (or throw if nColumns > nDefaultColumns)
/// * check that selected column names refer to valid branches, custom columns or datasource columns (throw if not)
/// Return the list of selected column names.
ColumnNames_t GetValidatedColumnNames(TLoopManager &lm, const unsigned int nColumns, const ColumnNames_t &columns,
                                      const ColumnNames_t &validCustomColumns, TDataSource *ds)
{
   const auto &defaultColumns = lm.GetDefaultColumnNames();
   const auto selectedColumns = SelectColumns(nColumns, columns, defaultColumns);
   const auto unknownColumns = FindUnknownColumns(selectedColumns, lm.GetTree(), validCustomColumns,
                                                  ds ? ds->GetColumnNames() : ColumnNames_t{});

   if (!unknownColumns.empty()) {
      // throw
      std::stringstream unknowns;
      std::string delim = unknownColumns.size() > 1 ? "s: " : ": "; // singular/plural
      for (auto &unknown : unknownColumns) {
         unknowns << delim << unknown;
         delim = ',';
      }
      throw std::runtime_error("Unknown column" + unknowns.str());
   }

   return selectedColumns;
}

/// Return a bitset each element of which indicates whether the corresponding element in `selectedColumns` is the
/// name of a column that must be defined via datasource. All elements of the returned vector are false if no
/// data-source is present.
std::vector<bool> FindUndefinedDSColumns(const ColumnNames_t &requestedCols, const ColumnNames_t &definedCols)
{
   const auto nColumns = requestedCols.size();
   std::vector<bool> mustBeDefined(nColumns, false);
   for (auto i = 0u; i < nColumns; ++i)
      mustBeDefined[i] = std::find(definedCols.begin(), definedCols.end(), requestedCols[i]) == definedCols.end();
   return mustBeDefined;
}

} // end ns TDF
} // end ns Internal
} // end ns ROOT
