/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <astGenerator.h>
#include <function.h>
#include "astGenerator.h"
#include "function.h"
#include "parserInt.h"
#include "parserUtil.h"
#include "queryInfoUtil.h"
#include "tbuffer.h"
#include "tglobal.h"
#include "tmsgtype.h"
#include "ttime.h"

#define TSQL_TBNAME_L "tbname"
#define DEFAULT_PRIMARY_TIMESTAMP_COL_NAME "_c0"
#define VALID_COLUMN_INDEX(index) (((index).tableIndex >= 0) && ((index).columnIndex >= TSDB_TBNAME_COLUMN_INDEX))

#define TSWINDOW_IS_EQUAL(t1, t2) (((t1).skey == (t2).skey) && ((t1).ekey == (t2).ekey))

// -1 is tbname column index, so here use the -2 as the initial value
#define COLUMN_INDEX_INITIAL_VAL (-2)
#define COLUMN_INDEX_INITIALIZER { COLUMN_INDEX_INITIAL_VAL, COLUMN_INDEX_INITIAL_VAL }

static int32_t validateSelectNodeList(SQueryStmtInfo* pQueryInfo, SArray* pSelNodeList, bool outerQuery, SMsgBuf* pMsgBuf);
static int32_t extractFunctionParameterInfo(SQueryStmtInfo* pQueryInfo, int32_t tokenId, STableMetaInfo** pTableMetaInfo, SSchema* columnSchema,
                                            tExprNode** pNode, SColumnIndex* pIndex, tSqlExprItem* pParamElem, SMsgBuf* pMsgBuf);

void setTokenAndResColumnName(tSqlExprItem* pItem, char* resColumnName, char* rawName, int32_t nameLength) {
  memset(resColumnName, 0, nameLength);

  int32_t len = ((int32_t)pItem->pNode->exprToken.n < nameLength) ? (int32_t)pItem->pNode->exprToken.n : nameLength;
  strncpy(rawName, pItem->pNode->exprToken.z, len);

  if (pItem->aliasName != NULL) {
    assert(strlen(pItem->aliasName) < nameLength);
    tstrncpy(resColumnName, pItem->aliasName, len);
  } else {
    strncpy(resColumnName, rawName, len);
  }
}

static int32_t evaluateSqlNodeImpl(tSqlExpr* pExpr, int32_t tsPrecision) {
  int32_t code = 0;
  if (pExpr->type == SQL_NODE_EXPR) {
    code = evaluateSqlNodeImpl(pExpr->pLeft, tsPrecision);
    if (code != TSDB_CODE_SUCCESS) {
      return code;
    }

    code = evaluateSqlNodeImpl(pExpr->pRight, tsPrecision);
    if (code != TSDB_CODE_SUCCESS) {
      return code;
    }

    if (pExpr->pLeft->type == SQL_NODE_VALUE && pExpr->pRight->type == SQL_NODE_VALUE) {
      tSqlExpr* pLeft  = pExpr->pLeft;
      tSqlExpr* pRight = pExpr->pRight;
      if ((pLeft->tokenId == TK_TIMESTAMP && (pRight->tokenId == TK_INTEGER || pRight->tokenId == TK_FLOAT)) ||
          ((pRight->tokenId == TK_TIMESTAMP && (pLeft->tokenId == TK_INTEGER || pLeft->tokenId == TK_FLOAT)))) {
        return TSDB_CODE_TSC_SQL_SYNTAX_ERROR;
      } else if (pLeft->tokenId == TK_TIMESTAMP && pRight->tokenId == TK_TIMESTAMP) {
        tSqlExprEvaluate(pExpr);
      } else {
        tSqlExprEvaluate(pExpr);
      }
    } else {
      // Other types of expressions are not evaluated, they will be handled during the validation of the abstract syntax tree.
    }
  } else if (pExpr->type == SQL_NODE_VALUE) {
    if (pExpr->tokenId == TK_NOW) {
      pExpr->value.i     = taosGetTimestamp(tsPrecision);
      pExpr->value.nType = TSDB_DATA_TYPE_BIGINT;
      pExpr->tokenId     = TK_TIMESTAMP;
    } else if (pExpr->tokenId == TK_VARIABLE) {
      char    unit = 0;
      SToken* pToken = &pExpr->exprToken;
      int32_t ret = parseAbsoluteDuration(pToken->z, pToken->n, &pExpr->value.i, &unit, tsPrecision);
      if (ret != TSDB_CODE_SUCCESS) {
        return TSDB_CODE_TSC_SQL_SYNTAX_ERROR;
      }

      pExpr->value.nType = TSDB_DATA_TYPE_BIGINT;
      pExpr->tokenId = TK_TIMESTAMP;
    }  else if (pExpr->tokenId == TK_NULL) {
      pExpr->value.nType = TSDB_DATA_TYPE_NULL;
    } else if (pExpr->tokenId == TK_INTEGER || pExpr->tokenId == TK_STRING || pExpr->tokenId == TK_FLOAT || pExpr->tokenId == TK_BOOL) {
      SToken* pToken = &pExpr->exprToken;

      int32_t tokenType = pToken->type;
      toTSDBType(tokenType);
      taosVariantCreate(&pExpr->value, pToken->z, pToken->n, tokenType);
    }

    return  TSDB_CODE_SUCCESS;
    // other types of data are handled in the parent level.
  } else if (pExpr->type == SQL_NODE_SQLFUNCTION) {
    SArray* pParam = pExpr->Expr.paramList;

    if (pParam != NULL) {
      for (int32_t i = 0; i < taosArrayGetSize(pParam); ++i) {
        tSqlExprItem* pItem = taosArrayGet(pParam, i);
        evaluateSqlNodeImpl(pItem->pNode, tsPrecision);
      }
    }
  }

  return  TSDB_CODE_SUCCESS;
}

void destroyFilterInfo(SColumnFilterList* pFilterList) {
  if (pFilterList->filterInfo == NULL) {
    pFilterList->numOfFilters = 0;
    return;
  }

  for(int32_t i = 0; i < pFilterList->numOfFilters; ++i) {
    if (pFilterList->filterInfo[i].filterstr) {
      tfree(pFilterList->filterInfo[i].pz);
    }
  }

  tfree(pFilterList->filterInfo);
  pFilterList->numOfFilters = 0;
}

void columnDestroy(SColumn* pCol) {
  destroyFilterInfo(&pCol->info.flist);
  free(pCol);
}

void destroyColumnList(SArray* pColumnList) {
  if (pColumnList == NULL) {
    return;
  }

  size_t num = taosArrayGetSize(pColumnList);
  for (int32_t i = 0; i < num; ++i) {
    SColumn* pCol = taosArrayGetP(pColumnList, i);
    columnDestroy(pCol);
  }

  taosArrayDestroy(pColumnList);
}

void clearTableMetaInfo(STableMetaInfo* pTableMetaInfo) {
  if (pTableMetaInfo == NULL) {
    return;
  }

  tfree(pTableMetaInfo->pTableMeta);
  tfree(pTableMetaInfo->vgroupList);

  destroyColumnList(pTableMetaInfo->tagColList);
  pTableMetaInfo->tagColList = NULL;

  free(pTableMetaInfo);
}

static STableMeta* extractTempTableMetaFromSubquery(SQueryStmtInfo* pUpstream) {
  STableMetaInfo* pUpstreamTableMetaInfo = getMetaInfo(pUpstream, 0);

  int32_t     numOfColumns = pUpstream->fieldsInfo.numOfOutput;
  STableMeta *meta = calloc(1, sizeof(STableMeta) + sizeof(SSchema) * numOfColumns);
  meta->tableType = TSDB_TEMP_TABLE;

  STableComInfo *info = &meta->tableInfo;
  info->numOfColumns = numOfColumns;
  info->precision    = pUpstreamTableMetaInfo->pTableMeta->tableInfo.precision;
  info->numOfTags    = 0;

  int32_t n = 0;
  for(int32_t i = 0; i < numOfColumns; ++i) {
    SInternalField* pField = getInternalField(&pUpstream->fieldsInfo, i);
    if (!pField->visible) {
      continue;
    }

    meta->schema[n] = pField->pExpr->base.resSchema;
    info->rowSize += meta->schema[n].bytes;
    n += 1;
  }

  info->numOfColumns = n;
  return meta;
}

SQueryStmtInfo *createQueryInfo() {
  SQueryStmtInfo* pQueryInfo = calloc(1, sizeof(SQueryStmtInfo));

  pQueryInfo->fieldsInfo.internalField = taosArrayInit(4, sizeof(SInternalField));
  pQueryInfo->exprList       = taosArrayInit(4, POINTER_BYTES);
  pQueryInfo->colList        = taosArrayInit(4, POINTER_BYTES);
  pQueryInfo->udColumnId     = TSDB_UD_COLUMN_INDEX;
  pQueryInfo->limit.limit    = -1;
  pQueryInfo->limit.offset   = 0;

  pQueryInfo->slimit.limit   = -1;
  pQueryInfo->slimit.offset  = 0;
  pQueryInfo->pUpstream      = taosArrayInit(4, POINTER_BYTES);
  pQueryInfo->window         = TSWINDOW_INITIALIZER;

  return pQueryInfo;
}

static void destroyQueryInfoImpl(SQueryStmtInfo* pQueryInfo) {
  cleanupTagCond(&pQueryInfo->tagCond);
  cleanupColumnCond(&pQueryInfo->colCond);
  cleanupFieldInfo(&pQueryInfo->fieldsInfo);

  dropAllExprInfo(pQueryInfo->exprList);
  pQueryInfo->exprList = NULL;

  if (pQueryInfo->exprList1 != NULL) {
    dropAllExprInfo(pQueryInfo->exprList1);
    pQueryInfo->exprList1 = NULL;
  }

  columnListDestroy(pQueryInfo->colList);
  pQueryInfo->colList = NULL;

  if (pQueryInfo->groupbyExpr.columnInfo != NULL) {
    taosArrayDestroy(pQueryInfo->groupbyExpr.columnInfo);
    pQueryInfo->groupbyExpr.columnInfo = NULL;
  }

  pQueryInfo->fillType = 0;

  tfree(pQueryInfo->fillVal);
  tfree(pQueryInfo->buf);

  taosArrayDestroy(pQueryInfo->pUpstream);
  pQueryInfo->pUpstream = NULL;
  pQueryInfo->bufLen = 0;
}

void destroyQueryInfo(SQueryStmtInfo* pQueryInfo) {
  while (pQueryInfo != NULL) {
    SQueryStmtInfo* p = pQueryInfo->sibling;

    size_t numOfUpstream = taosArrayGetSize(pQueryInfo->pUpstream);
    for (int32_t i = 0; i < numOfUpstream; ++i) {
      SQueryStmtInfo* pUpQueryInfo = taosArrayGetP(pQueryInfo->pUpstream, i);
      destroyQueryInfoImpl(pUpQueryInfo);
      clearAllTableMetaInfo(pUpQueryInfo, false, 0);
      tfree(pUpQueryInfo);
    }

    destroyQueryInfoImpl(pQueryInfo);
    clearAllTableMetaInfo(pQueryInfo, false, 0);
    tfree(pQueryInfo);
    pQueryInfo = p;
  }
}

static int32_t doValidateSubquery(SSqlNode* pSqlNode, int32_t index, SQueryStmtInfo* pQueryInfo, SMsgBuf* pMsgBuf) {
  SRelElementPair* subInfo = taosArrayGet(pSqlNode->from->list, index);

  // union all is not support currently
  SSqlNode* p = taosArrayGetP(subInfo->pSubquery, 0);
  if (taosArrayGetSize(subInfo->pSubquery) >= 2) {
    return buildInvalidOperationMsg(pMsgBuf, "not support union in subquery");
  }

  SQueryStmtInfo* pSub = createQueryInfo();

  SArray *pUdfInfo = NULL;
  if (pQueryInfo->pUdfInfo) {
    pUdfInfo = taosArrayDup(pQueryInfo->pUdfInfo);
  }

  pSub->pUdfInfo = pUdfInfo;
  pSub->pDownstream = pQueryInfo;
  int32_t code = validateSqlNode(p, pSub, pMsgBuf);
  if (code != TSDB_CODE_SUCCESS) {
    return code;
  }

  // create dummy table meta info
  STableMetaInfo* pTableMetaInfo1 = calloc(1, sizeof(STableMetaInfo));
  if (pTableMetaInfo1 == NULL) {
    return TSDB_CODE_TSC_OUT_OF_MEMORY;
  }

  pTableMetaInfo1->pTableMeta = extractTempTableMetaFromSubquery(pSub);

  if (subInfo->aliasName.n > 0) {
    if (subInfo->aliasName.n >= TSDB_TABLE_FNAME_LEN) {
      tfree(pTableMetaInfo1);
      return buildInvalidOperationMsg(pMsgBuf, "subquery alias name too long");
    }

    tstrncpy(pTableMetaInfo1->aliasName, subInfo->aliasName.z, subInfo->aliasName.n + 1);
  }

  taosArrayPush(pQueryInfo->pUpstream, &pSub);

  // NOTE: order mix up in subquery not support yet.
  pQueryInfo->order = pSub->order;

  STableMetaInfo** tmp = realloc(pQueryInfo->pTableMetaInfo, (pQueryInfo->numOfTables + 1) * POINTER_BYTES);
  if (tmp == NULL) {
    tfree(pTableMetaInfo1);
    return TSDB_CODE_TSC_OUT_OF_MEMORY;
  }

  pQueryInfo->pTableMetaInfo = tmp;

  pQueryInfo->pTableMetaInfo[pQueryInfo->numOfTables] = pTableMetaInfo1;
  pQueryInfo->numOfTables += 1;

  // all columns are added into the table column list
  STableMeta* pMeta = pTableMetaInfo1->pTableMeta;
  int32_t startOffset = (int32_t) taosArrayGetSize(pQueryInfo->colList);

  for(int32_t i = 0; i < pMeta->tableInfo.numOfColumns; ++i) {
    columnListInsert(pQueryInfo->colList, i + startOffset, pMeta->uid, &pMeta->schema[i]);
  }

  return TSDB_CODE_SUCCESS;
}

int32_t getTableIndexImpl(SToken* pTableToken, SQueryStmtInfo* pQueryInfo, SColumnIndex* pIndex) {
  if (pTableToken->n == 0) {  // only one table and no table name prefix in column name
    if (pQueryInfo->numOfTables == 1) {
      pIndex->tableIndex = 0;
    } else {
      pIndex->tableIndex = COLUMN_INDEX_INITIAL_VAL;
    }

    return TSDB_CODE_SUCCESS;
  }

  pIndex->tableIndex = COLUMN_INDEX_INITIAL_VAL;
  for (int32_t i = 0; i < pQueryInfo->numOfTables; ++i) {
    STableMetaInfo* pTableMetaInfo = getMetaInfo(pQueryInfo, i);
    char* name = pTableMetaInfo->aliasName;
    if (strncasecmp(name, pTableToken->z, pTableToken->n) == 0 && strlen(name) == pTableToken->n) {
      pIndex->tableIndex = i;
      return TSDB_CODE_SUCCESS;
    }
  }

  return TSDB_CODE_TSC_INVALID_OPERATION;
}

void extractTableNameFromToken(SToken* pToken, SToken* pTable) {
  const char sep = TS_PATH_DELIMITER[0];

  if (pToken == pTable || pToken == NULL || pTable == NULL) {
    return;
  }

  char* r = strnchr(pToken->z, sep, pToken->n, false);

  if (r != NULL) {  // record the table name token
    pTable->n = (uint32_t)(r - pToken->z);
    pTable->z = pToken->z;

    r += 1;
    pToken->n -= (uint32_t)(r - pToken->z);
    pToken->z = r;
  }
}

int32_t getTableIndexByName(SToken* pToken, SQueryStmtInfo* pQueryInfo, SColumnIndex* pIndex) {
  SToken tableToken = {0};
  extractTableNameFromToken(pToken, &tableToken);

  if (getTableIndexImpl(&tableToken, pQueryInfo, pIndex) != TSDB_CODE_SUCCESS) {
    return TSDB_CODE_TSC_INVALID_OPERATION;
  }

  return TSDB_CODE_SUCCESS;
}

static int16_t doGetColumnIndex(SQueryStmtInfo* pQueryInfo, int32_t index, const SToken* pToken, int16_t* type) {
  STableMeta* pTableMeta = getMetaInfo(pQueryInfo, index)->pTableMeta;

  int32_t  numOfCols = getNumOfColumns(pTableMeta) + getNumOfTags(pTableMeta);
  SSchema* pSchema = getTableColumnSchema(pTableMeta);

  int16_t columnIndex = COLUMN_INDEX_INITIAL_VAL;

  for (int32_t i = 0; i < numOfCols; ++i) {
    if (pToken->n != strlen(pSchema[i].name)) {
      continue;
    }

    if (strncasecmp(pSchema[i].name, pToken->z, pToken->n) == 0) {
      columnIndex = i;
      break;
    }
  }

  *type = (columnIndex >= getNumOfColumns(pTableMeta))? TSDB_COL_TAG:TSDB_COL_NORMAL;
  return columnIndex;
}

static bool isTablenameToken(SToken* token) {
  SToken tmpToken = *token;
  SToken tableToken = {0};

  extractTableNameFromToken(&tmpToken, &tableToken);
  return (tmpToken.n == strlen(TSQL_TBNAME_L) && strncasecmp(TSQL_TBNAME_L, tmpToken.z, tmpToken.n) == 0);
}

int32_t doGetColumnIndexByName(SToken* pToken, SQueryStmtInfo* pQueryInfo, SColumnIndex* pIndex, SMsgBuf* pMsgBuf) {
  const char* msg0 = "ambiguous column name";
  const char* msg1 = "invalid column name";

  pIndex->type = TSDB_COL_NORMAL;

  if (isTablenameToken(pToken)) {
    pIndex->columnIndex = TSDB_TBNAME_COLUMN_INDEX;
    pIndex->type = TSDB_COL_TAG;
  } else if (strlen(DEFAULT_PRIMARY_TIMESTAMP_COL_NAME) == pToken->n &&
             strncasecmp(pToken->z, DEFAULT_PRIMARY_TIMESTAMP_COL_NAME, pToken->n) == 0) {
    pIndex->columnIndex = PRIMARYKEY_TIMESTAMP_COL_ID; // just make runtime happy, need fix java test case InsertSpecialCharacterJniTest
  } else if (pToken->n == 0) {
    pIndex->columnIndex = PRIMARYKEY_TIMESTAMP_COL_ID; // just make runtime happy, need fix java test case InsertSpecialCharacterJniTest
  } else {
    // not specify the table name, try to locate the table index by column name
    if (pIndex->tableIndex == COLUMN_INDEX_INITIAL_VAL) {
      for (int16_t i = 0; i < pQueryInfo->numOfTables; ++i) {
        int16_t colIndex = doGetColumnIndex(pQueryInfo, i, pToken, &pIndex->type);

        if (colIndex != COLUMN_INDEX_INITIAL_VAL) {
          if (pIndex->columnIndex != COLUMN_INDEX_INITIAL_VAL) {
            return buildInvalidOperationMsg(pMsgBuf, msg0);
          } else {
            pIndex->tableIndex = i;
            pIndex->columnIndex = colIndex;
          }
        }
      }
    } else {  // table index is valid, get the column index
      pIndex->columnIndex = doGetColumnIndex(pQueryInfo, pIndex->tableIndex, pToken, &pIndex->type);
    }

    if (pIndex->columnIndex == COLUMN_INDEX_INITIAL_VAL) {
      return buildInvalidOperationMsg(pMsgBuf, msg1);
    }
  }

  if (VALID_COLUMN_INDEX(*pIndex)) {
    return TSDB_CODE_SUCCESS;
  } else {
    return TSDB_CODE_TSC_INVALID_OPERATION;
  }
}

int32_t getColumnIndexByName(const SToken* pToken, SQueryStmtInfo* pQueryInfo, SColumnIndex* pIndex, SMsgBuf* pMsgBuf) {
  if (pQueryInfo->pTableMetaInfo == NULL || pQueryInfo->numOfTables == 0) {
    return TSDB_CODE_TSC_INVALID_OPERATION;
  }

  SToken tmpToken = *pToken;
  if (getTableIndexByName(&tmpToken, pQueryInfo, pIndex) != TSDB_CODE_SUCCESS) {
    return TSDB_CODE_TSC_INVALID_OPERATION;
  }

  return doGetColumnIndexByName(&tmpToken, pQueryInfo, pIndex, pMsgBuf);
}

int32_t validateGroupbyNode(SQueryStmtInfo* pQueryInfo, SArray* pList, SMsgBuf* pMsgBuf) {
  const char* msg1 = "too many columns in group by clause";
  const char* msg2 = "invalid column name in group by clause";
  const char* msg3 = "columns from one table allowed as group by columns";
  const char* msg4 = "join query does not support group by";
  const char* msg5 = "not allowed column type for group by";
  const char* msg6 = "tags not allowed for table query";
  const char* msg7 = "normal column and tags can not be mixed up in group by clause";
  const char* msg8 = "normal column can only locate at the end of group by clause";

  SGroupbyExpr* pGroupExpr = &(pQueryInfo->groupbyExpr);
  pGroupExpr->columnInfo = taosArrayInit(4, sizeof(SColIndex));
  if (pGroupExpr->columnInfo == NULL) {
    return TSDB_CODE_TSC_OUT_OF_MEMORY;
  }

  // todo : handle two tables situation
  STableMetaInfo* pTableMetaInfo = NULL;
  if (pList == NULL) {
    return TSDB_CODE_SUCCESS;
  }

  if (pQueryInfo->numOfTables > 1) {
    return buildInvalidOperationMsg(pMsgBuf, msg4);
  }

  size_t num = taosArrayGetSize(pList);
  if (num > TSDB_MAX_TAGS) {
    return buildInvalidOperationMsg(pMsgBuf, msg1);
  }

  int32_t  numOfGroupbyCols = 0;
  SSchema *pSchema          = NULL;
  int32_t  tableIndex       = COLUMN_INDEX_INITIAL_VAL;
  bool groupbyTag           = false;

  for (int32_t i = 0; i < num; ++i) {
    SListItem * pItem = taosArrayGet(pList, i);
    SVariant* pVar = &pItem->pVar;

    SColumnIndex index = COLUMN_INDEX_INITIALIZER;
    SToken token = {pVar->nLen, pVar->nType, pVar->pz};
    if (getColumnIndexByName(&token, pQueryInfo, &index, pMsgBuf) != TSDB_CODE_SUCCESS) {
      return buildInvalidOperationMsg(pMsgBuf, msg2);
    }

    // Group by multiple tables is not supported.
    if (tableIndex == COLUMN_INDEX_INITIAL_VAL) {
      tableIndex = index.tableIndex;
    } else if (tableIndex != index.tableIndex) {
      return buildInvalidOperationMsg(pMsgBuf, msg3);
    }

    pTableMetaInfo = getMetaInfo(pQueryInfo, index.tableIndex);
    STableMeta* pTableMeta = pTableMetaInfo->pTableMeta;

    if (index.columnIndex == TSDB_TBNAME_COLUMN_INDEX) {
      pSchema = getTbnameColumnSchema();
    } else {
      pSchema = getOneColumnSchema(pTableMeta, index.columnIndex);
    }

    bool groupTag = TSDB_COL_IS_TAG(index.type);
    if (groupTag) {
      if (!UTIL_TABLE_IS_SUPER_TABLE(pTableMetaInfo)) {
        return buildInvalidOperationMsg(pMsgBuf, msg6);
      }

      groupbyTag = true;

      int32_t relIndex = index.columnIndex;
      if (index.columnIndex != TSDB_TBNAME_COLUMN_INDEX) {
        relIndex -= getNumOfColumns(pTableMeta);
      }

      SColIndex colIndex = { .colIndex = relIndex, .flag = TSDB_COL_TAG, .colId = pSchema->colId, };
      strncpy(colIndex.name, pSchema->name, tListLen(colIndex.name));
      taosArrayPush(pGroupExpr->columnInfo, &colIndex);

      index.columnIndex = relIndex;
      columnListInsert(pTableMetaInfo->tagColList, index.columnIndex, pTableMeta->uid, pSchema);
    } else {
      // check if the column type is valid, here only support the bool/tinyint/smallint/bigint group by
      if (pSchema->type == TSDB_DATA_TYPE_FLOAT || pSchema->type == TSDB_DATA_TYPE_DOUBLE) {
        return buildInvalidOperationMsg(pMsgBuf, msg5);
      }

      columnListInsert(pQueryInfo->colList, index.columnIndex, pTableMeta->uid, pSchema);

      SColIndex colIndex = { .colIndex = index.columnIndex, .flag = TSDB_COL_NORMAL, .colId = pSchema->colId };
      strncpy(colIndex.name, pSchema->name, tListLen(colIndex.name));

      taosArrayPush(pGroupExpr->columnInfo, &colIndex);

      numOfGroupbyCols++;
      pQueryInfo->info.groupbyColumn = true;
    }
  }

  if (numOfGroupbyCols > 0 && groupbyTag) {
    return buildInvalidOperationMsg(pMsgBuf, msg7);
  }

  // todo ???
  // 1. the normal column in the group by clause can only located at the end position
  for(int32_t i = 0; i < num; ++i) {
    SColIndex* pIndex = taosArrayGet(pGroupExpr->columnInfo, i);
    if (TSDB_COL_IS_NORMAL_COL(pIndex->flag) && i != num - 1) {
      return buildInvalidOperationMsg(pMsgBuf, msg8);
    }
  }

  pGroupExpr->orderType  = TSDB_ORDER_ASC;
  pGroupExpr->tableIndex = tableIndex;
  return TSDB_CODE_SUCCESS;
}

int32_t checkForUnsupportedQuery(SQueryStmtInfo* pQueryInfo, SMsgBuf* pMsgBuf) {
  const char* msg1 = "not support percentile/interp/block_dist in the outer query yet";

  for (int32_t i = 0; i < getNumOfExprs(pQueryInfo); ++i) {
    SExprInfo* pExpr = getExprInfo(pQueryInfo, i);
    assert(pExpr->pExpr->nodeType == TEXPR_UNARYEXPR_NODE);

    int32_t f = getExprFunctionId(pExpr);
    if (f == FUNCTION_PERCT || f == FUNCTION_INTERP) {
      return buildInvalidOperationMsg(pMsgBuf, msg1);
    }

    if (f == FUNCTION_BLKINFO && taosArrayGetSize(pQueryInfo->pUpstream) > 0) {
      return buildInvalidOperationMsg(pMsgBuf, msg1);
    }

#if 0
    //todo planner handle this
    if (/*(timeWindowQuery || pQueryInfo->stateWindow) &&*/ f == FUNCTION_LAST) {
      pExpr->base.numOfParams = 1;
      pExpr->base.param[0].i = TSDB_ORDER_ASC;
      pExpr->base.param[0].nType = TSDB_DATA_TYPE_INT;
    }
#endif
  }
}

int32_t validateWhereNode(SQueryStmtInfo *pQueryInfo, tSqlExpr* pWhereExpr, SMsgBuf* pMsgBuf) {
  return 0;
}

static int32_t parseIntervalOffset(SQueryStmtInfo* pQueryInfo, SToken* offsetToken, int32_t precision, SMsgBuf* pMsgBuf) {
  const char* msg1 = "interval offset cannot be negative";
  const char* msg2 = "interval offset should be shorter than interval";
  const char* msg3 = "cannot use 'year' as offset when interval is 'month'";

  SToken* t = offsetToken;
  SInterval* pInterval = &pQueryInfo->interval;

  if (t->n == 0) {
    pInterval->offsetUnit = pInterval->intervalUnit;
    pInterval->offset = 0;
    return TSDB_CODE_SUCCESS;
  }

  if (parseNatualDuration(t->z, t->n, &pInterval->offset, &pInterval->offsetUnit, precision) != TSDB_CODE_SUCCESS) {
    return TSDB_CODE_TSC_INVALID_OPERATION;
  }

  if (pInterval->offset < 0) {
    return buildInvalidOperationMsg(pMsgBuf, msg1);
  }

  if (!TIME_IS_VAR_DURATION(pInterval->offsetUnit)) {
    if (!TIME_IS_VAR_DURATION(pInterval->intervalUnit)) {
      if (pInterval->offset > pInterval->interval) {
        return buildInvalidOperationMsg(pMsgBuf, msg2);
      }
    }
  } else if (pInterval->offsetUnit == pInterval->intervalUnit) {
    if (pInterval->offset >= pInterval->interval) {
      return buildInvalidOperationMsg(pMsgBuf, msg2);
    }
  } else if (pInterval->intervalUnit == 'n' && pInterval->offsetUnit == 'y') {
    return buildInvalidOperationMsg(pMsgBuf, msg3);
  } else if (pInterval->intervalUnit == 'y' && pInterval->offsetUnit == 'n') {
    if (pInterval->interval * 12 <= pQueryInfo->interval.offset) {
      return buildInvalidOperationMsg(pMsgBuf, msg2);
    }
  } else {
    // TODO: offset should be shorter than interval, but how to check
    // conflicts like 30days offset and 1 month interval
  }

  return TSDB_CODE_SUCCESS;
}

static int32_t parseSlidingClause(SQueryStmtInfo* pQueryInfo, SToken* pSliding, int32_t precision, SMsgBuf* pMsgBuf) {
  const char* msg1 = "sliding value no larger than the interval value";
  const char* msg2 = "sliding value can not less than 1% of interval value";
  const char* msg3 = "does not support sliding when interval is natural month/year";
  const char* msg4 = "sliding value too small";

  const static int32_t INTERVAL_SLIDING_FACTOR = 100;

  SInterval* pInterval = &pQueryInfo->interval;
  if (pSliding->n == 0) {
    pInterval->slidingUnit = pInterval->intervalUnit;
    pInterval->sliding     = pInterval->interval;
    return TSDB_CODE_SUCCESS;
  }

  if (TIME_IS_VAR_DURATION(pInterval->intervalUnit)) {
    return buildInvalidOperationMsg(pMsgBuf, msg3);
  }

  parseAbsoluteDuration(pSliding->z, pSliding->n, &pInterval->sliding, &pInterval->slidingUnit, precision);

  // less than the threshold
  if (pInterval->sliding < convertTimePrecision(tsMinSlidingTime, TSDB_TIME_PRECISION_MILLI, precision)) {
    return buildInvalidOperationMsg(pMsgBuf, msg4);
  }

  if (pInterval->sliding > pInterval->interval) {
    return buildInvalidOperationMsg(pMsgBuf, msg1);
  }

  if ((pInterval->interval != 0) && (pInterval->interval/pInterval->sliding > INTERVAL_SLIDING_FACTOR)) {
    return buildInvalidOperationMsg(pMsgBuf, msg2);
  }

  return TSDB_CODE_SUCCESS;
}

// validate the interval info
int32_t validateIntervalNode(SQueryStmtInfo *pQueryInfo, SSqlNode* pSqlNode, SMsgBuf* pMsgBuf) {
  const char* msg1 = "sliding cannot be used without interval";
  const char* msg2 = "only point interpolation query requires keyword EVERY";
  const char* msg3 = "interval value is too small";

  STableMetaInfo* pTableMetaInfo = getMetaInfo(pQueryInfo, 0);
  STableComInfo tinfo = getTableInfo(pTableMetaInfo->pTableMeta);

  if (!TPARSER_HAS_TOKEN(pSqlNode->interval.interval)) {
    if (TPARSER_HAS_TOKEN(pSqlNode->sliding)) {
      return buildInvalidOperationMsg(pMsgBuf, msg1);
    } else {
      return TSDB_CODE_SUCCESS;
    }
  }

  // orderby column not set yet, set it to be the primary timestamp column
  if (pQueryInfo->order.orderColId == INT32_MIN) {
    pQueryInfo->order.orderColId = PRIMARYKEY_TIMESTAMP_COL_ID;
  }

  // interval is not null
  SToken *t = &pSqlNode->interval.interval;
  if (parseNatualDuration(t->z, t->n, &pQueryInfo->interval.interval,
                          &pQueryInfo->interval.intervalUnit, tinfo.precision) != TSDB_CODE_SUCCESS) {
    return TSDB_CODE_TSC_INVALID_OPERATION;
  }

  if (pQueryInfo->interval.interval <= 0) {
    return buildInvalidOperationMsg(pMsgBuf, msg3);
  }

  if (!TIME_IS_VAR_DURATION(pQueryInfo->interval.intervalUnit)) {
    // interval cannot be less than 10 milliseconds
    if (convertTimePrecision(pQueryInfo->interval.interval, tinfo.precision, TSDB_TIME_PRECISION_MICRO) < tsMinIntervalTime) {
      char msg[50] = {0};
      snprintf(msg, 50, "interval time window can not be less than %d %s", tsMinIntervalTime, TSDB_TIME_PRECISION_MICRO_STR);
      return buildInvalidOperationMsg(pMsgBuf, msg);
    }
  }

  if (parseIntervalOffset(pQueryInfo, &pSqlNode->interval.offset, tinfo.precision, pMsgBuf) != TSDB_CODE_SUCCESS) {
    return TSDB_CODE_TSC_INVALID_OPERATION;
  }

  if (parseSlidingClause(pQueryInfo, &pSqlNode->sliding, tinfo.precision, pMsgBuf) != TSDB_CODE_SUCCESS) {
    return TSDB_CODE_TSC_INVALID_OPERATION;
  }

  // It is a time window query
  pQueryInfo->info.timewindow = true;
  return TSDB_CODE_SUCCESS;
  // disable it temporarily
//  bool interpQuery = tscIsPointInterpQuery(pQueryInfo);
//  if ((pSqlNode->interval.token == TK_EVERY && (!interpQuery)) || (pSqlNode->interval.token == TK_INTERVAL && interpQuery)) {
//    return buildInvalidOperationMsg(pMsgBuf, msg4);
//  }
}

int32_t validateSessionNode(SQueryStmtInfo *pQueryInfo, SSessionWindowVal* pSession, int32_t precision, SMsgBuf* pMsgBuf) {
  const char* msg1 = "gap should be fixed time window";
  const char* msg2 = "only one type time window allowed";
  const char* msg3 = "invalid column name";
  const char* msg4 = "invalid time window";

  // no session window
  if (!TPARSER_HAS_TOKEN(pSession->gap)) {
    return TSDB_CODE_SUCCESS;
  }

  SToken* col = &pSession->col;
  SToken* gap = &pSession->gap;

  char timeUnit = 0;
  if (parseNatualDuration(gap->z, gap->n, &pQueryInfo->sessionWindow.gap, &timeUnit, precision) != TSDB_CODE_SUCCESS) {
    return buildInvalidOperationMsg(pMsgBuf, msg4);
  }

  if (TIME_IS_VAR_DURATION(timeUnit)) {
    return buildInvalidOperationMsg(pMsgBuf, msg1);
  }

  if (pQueryInfo->sessionWindow.gap != 0 && pQueryInfo->interval.interval != 0) {
    return buildInvalidOperationMsg(pMsgBuf, msg2);
  }

  if (pQueryInfo->sessionWindow.gap == 0) {
    return buildInvalidOperationMsg(pMsgBuf, msg4);
  }

  SColumnIndex index = COLUMN_INDEX_INITIALIZER;
  if ((getColumnIndexByName(col, pQueryInfo, &index, pMsgBuf) != TSDB_CODE_SUCCESS)) {
    return buildInvalidOperationMsg(pMsgBuf, msg3);
  }

  if (index.columnIndex != PRIMARYKEY_TIMESTAMP_COL_ID) {
    return buildInvalidOperationMsg(pMsgBuf, msg3);
  }

  pQueryInfo->sessionWindow.primaryColId = PRIMARYKEY_TIMESTAMP_COL_ID;
  return TSDB_CODE_SUCCESS;
}

// parse the window_state
int32_t validateStateWindowNode(SQueryStmtInfo *pQueryInfo, SWindowStateVal* pWindowState, SMsgBuf* pMsgBuf) {
  const char* msg1 = "invalid column name";
  const char* msg2 = "invalid column type";
  const char* msg3 = "not support state_window with group by ";
  const char* msg4 = "function not support for super table query";
  const char* msg5 = "not support state_window on tag column";

  SToken *col = &(pWindowState->col) ;
  if (!TPARSER_HAS_TOKEN(*col)) {
    return TSDB_CODE_SUCCESS;
  }

  SGroupbyExpr* pGroupExpr = &pQueryInfo->groupbyExpr;
  if (taosArrayGetSize(pGroupExpr->columnInfo) > 0) {
    return buildInvalidOperationMsg(pMsgBuf, msg3);
  }

  SColumnIndex index = COLUMN_INDEX_INITIALIZER;
  if (getColumnIndexByName(col, pQueryInfo, &index, pMsgBuf) !=  TSDB_CODE_SUCCESS) {
    return buildInvalidOperationMsg(pMsgBuf, msg1);
  }

  STableMetaInfo *pTableMetaInfo = getMetaInfo(pQueryInfo, index.tableIndex);
  STableMeta* pTableMeta = pTableMetaInfo->pTableMeta;

  if (UTIL_TABLE_IS_SUPER_TABLE(pTableMetaInfo)) {
    return buildInvalidOperationMsg(pMsgBuf, msg4);
  }

  if (TSDB_COL_IS_TAG(index.type)) {
    return buildInvalidOperationMsg(pMsgBuf, msg5);
  }

  if (pGroupExpr->columnInfo == NULL) {
    pGroupExpr->columnInfo = taosArrayInit(4, sizeof(SColIndex));
  }

  SSchema* pSchema = getOneColumnSchema(pTableMeta, index.columnIndex);
  if (pSchema->type == TSDB_DATA_TYPE_TIMESTAMP || IS_FLOAT_TYPE(pSchema->type)) {
    return buildInvalidOperationMsg(pMsgBuf, msg2);
  }

  columnListInsert(pQueryInfo->colList, index.columnIndex, pTableMeta->uid, pSchema);
  SColIndex colIndex = { .colIndex = index.columnIndex, .flag = TSDB_COL_NORMAL, .colId = pSchema->colId };

  //TODO use group by routine? state window query not support stable query.
  taosArrayPush(pGroupExpr->columnInfo, &colIndex);
  pGroupExpr->orderType = TSDB_ORDER_ASC;
  pQueryInfo->info.stateWindow = true;

  return TSDB_CODE_SUCCESS;
}

// parse the having clause in the first place
int32_t validateHavingNode(SQueryStmtInfo *pQueryInfo, SSqlNode* pSqlNode, SMsgBuf* pMsgBuf) {
  return 0;
}

int32_t validateLimitNode(SQueryStmtInfo *pQueryInfo, SSqlNode* pSqlNode, SMsgBuf* pMsgBuf) {
  STableMetaInfo* pTableMetaInfo = getMetaInfo(pQueryInfo, 0);

  const char* msg1 = "slimit/soffset only available for STable query";
  const char* msg2 = "slimit/soffset can not apply to projection query";
  const char* msg3 = "soffset/offset can not be less than 0";

  // handle the limit offset value, validate the limit
  pQueryInfo->limit = pSqlNode->limit;
  pQueryInfo->slimit = pSqlNode->slimit;

//  tscDebug("0x%"PRIx64" limit:%" PRId64 ", offset:%" PRId64 " slimit:%" PRId64 ", soffset:%" PRId64, pSql->self,
//           pQueryInfo->limit.limit, pQueryInfo->limit.offset, pQueryInfo->slimit.limit, pQueryInfo->slimit.offset);

  if (pQueryInfo->slimit.offset < 0 || pQueryInfo->limit.offset < 0) {
    return buildInvalidOperationMsg(pMsgBuf, msg3);
  }

  if (pQueryInfo->limit.limit == 0) {
//    tscDebug("0x%"PRIx64" limit 0, no output result", pSql->self);
    pQueryInfo->command = TSDB_SQL_RETRIEVE_EMPTY_RESULT;
    return TSDB_CODE_SUCCESS;
  }

  if (UTIL_TABLE_IS_SUPER_TABLE(pTableMetaInfo)) {
//    if (!tscQueryTags(pQueryInfo)) {  // local handle the super table tag query
//      if (tscIsProjectionQueryOnSTable(pQueryInfo, 0)) {
//        if (pQueryInfo->slimit.limit > 0 || pQueryInfo->slimit.offset > 0) {
//          return buildInvalidOperationMsg(pMsgBuf, msg2);
//        }
//
//        // for projection query on super table, all queries are subqueries
//        if (tscNonOrderedProjectionQueryOnSTable(pQueryInfo, 0) &&
//            !TSDB_QUERY_HAS_TYPE(pQueryInfo->type, TSDB_QUERY_TYPE_JOIN_QUERY)) {
//          pQueryInfo->type |= TSDB_QUERY_TYPE_SUBQUERY;
//        }
//      }
//    }

    if (pQueryInfo->slimit.limit == 0) {
//      tscDebug("0x%"PRIx64" slimit 0, no output result", pSql->self);
      pQueryInfo->command = TSDB_SQL_RETRIEVE_EMPTY_RESULT;
      return TSDB_CODE_SUCCESS;
    }
    
    // No tables included. No results generated. Query results are empty.
    if (pTableMetaInfo->vgroupList->numOfVgroups == 0) {
//      tscDebug("0x%"PRIx64" no table in super table, no output result", pSql->self);
      pQueryInfo->command = TSDB_SQL_RETRIEVE_EMPTY_RESULT;
      return TSDB_CODE_SUCCESS;
    }
  } else {
    if (pQueryInfo->slimit.limit != -1 || pQueryInfo->slimit.offset != 0) {
      return buildInvalidOperationMsg(pMsgBuf, msg1);
    }
  }
}

static void setTsOutputExprInfo(SQueryStmtInfo* pQueryInfo, STableMetaInfo* pTableMetaInfo, int32_t outputIndex, int32_t tableIndex);

int32_t validateOrderbyNode(SQueryStmtInfo *pQueryInfo, SSqlNode* pSqlNode, SMsgBuf* pMsgBuf) {
  const char* msg1 = "invalid column name in orderby clause";
  const char* msg2 = "too many order by columns";
  const char* msg3 = "only one column allowed in orderby";
  const char* msg4 = "invalid order by column index";

  if (pSqlNode->pSortOrder == NULL) {
    return TSDB_CODE_SUCCESS;
  }

  STableMetaInfo* pTableMetaInfo = getMetaInfo(pQueryInfo, 0);
  SArray* pSortOrder = pSqlNode->pSortOrder;

  /*
   * for table query, there is only one or none order option is allowed, which is the
   * ts or values(top/bottom) order is supported.
   *
   * for super table query, the order option must be less than 3.
   */
  size_t size = taosArrayGetSize(pSortOrder);
  if (UTIL_TABLE_IS_NORMAL_TABLE(pTableMetaInfo) || UTIL_TABLE_IS_TMP_TABLE(pTableMetaInfo)) {
    if (size > 1) {
      return buildInvalidOperationMsg(pMsgBuf, msg3);
    }
  } else {
    if (size > 2) {
      return buildInvalidOperationMsg(pMsgBuf, msg2);
    }
  }

  // handle the first part of order by
  SVariant* pVar = taosArrayGet(pSortOrder, 0);
  SSchema s = {0};
  if (pVar->nType == TSDB_DATA_TYPE_BINARY) {
    SColumnIndex index = COLUMN_INDEX_INITIALIZER;
    SToken columnName = {pVar->nLen, pVar->nType, pVar->pz};
    if (getColumnIndexByName(&columnName, pQueryInfo, &index, pMsgBuf) != TSDB_CODE_SUCCESS) {
      return buildInvalidOperationMsg(pMsgBuf, msg1);
    }

    s = *(SSchema*) getOneColumnSchema(pTableMetaInfo->pTableMeta, index.columnIndex);
  } else { // order by [1|2|3]
    if (pVar->i > getNumOfFields(&pQueryInfo->fieldsInfo)) {
      return buildInvalidOperationMsg(pMsgBuf, msg4);
    }

    SExprInfo* pExprInfo = getExprInfo(pQueryInfo, pVar->i);
    s = pExprInfo->base.resSchema;
  }

  SListItem* pItem = taosArrayGet(pSqlNode->pSortOrder, 0);
  pQueryInfo->order.order = pItem->sortOrder;
  pQueryInfo->order.orderColId = s.colId;

  return TSDB_CODE_SUCCESS;
}

#if 0
// set order by info
int32_t checkForInvalidOrderby(SQueryStmtInfo *pQueryInfo, SSqlNode* pSqlNode, SMsgBuf* pMsgBuf) {
  const char* msg0 = "only one column allowed in orderby";
  const char* msg1 = "invalid column name in orderby clause";
  const char* msg2 = "too many order by columns";
  const char* msg3 = "only primary timestamp/tbname/first tag in groupby clause allowed";
  const char* msg4 = "only tag in groupby clause allowed in order clause";
  const char* msg5 = "only primary timestamp/column in top/bottom function allowed as order column";
  const char* msg6 = "only primary timestamp allowed as the second order column";
  const char* msg7 = "only primary timestamp/column in groupby clause allowed as order column";
  const char* msg8 = "only column in groupby clause allowed as order column";
  const char* msg9 = "orderby column must projected in subquery";
  const char* msg10 = "not support distinct mixed with order by";

//  setDefaultOrderInfo(pQueryInfo);
  STableMetaInfo* pTableMetaInfo = getMetaInfo(pQueryInfo, 0);
  SSchema* pSchema = getTableColumnSchema(pTableMetaInfo->pTableMeta);
  int32_t numOfCols = getNumOfColumns(pTableMetaInfo->pTableMeta);

  if (pSqlNode->pSortOrder == NULL) {
    return TSDB_CODE_SUCCESS;
  }

  SArray* pSortOrder = pSqlNode->pSortOrder;

  /*
   * for table query, there is only one or none order option is allowed, which is the
   * ts or values(top/bottom) order is supported.
   *
   * for super table query, the order option must be less than 3.
   */
  size_t size = taosArrayGetSize(pSortOrder);
  if (UTIL_TABLE_IS_NORMAL_TABLE(pTableMetaInfo) || UTIL_TABLE_IS_TMP_TABLE(pTableMetaInfo)) {
    if (size > 1) {
      return buildInvalidOperationMsg(pMsgBuf, msg0);
    }
  } else {
    if (size > 2) {
      return buildInvalidOperationMsg(pMsgBuf, msg2);
    }
  }

#if 0
  if (size > 0 && pQueryInfo->distinct) {
    return buildInvalidOperationMsg(pMsgBuf, msg10);
  }
#endif

  // handle the first part of order by
  SVariant* pVar = taosArrayGet(pSortOrder, 0);

#if 0
  // e.g., order by 1 asc, return directly with out further check.
  if (pVar->nType >= TSDB_DATA_TYPE_TINYINT && pVar->nType <= TSDB_DATA_TYPE_BIGINT) {
    return TSDB_CODE_SUCCESS;
  }
#endif

  SToken columnName = {pVar->nLen, pVar->nType, pVar->pz};

  SColumnIndex index = COLUMN_INDEX_INITIALIZER;
  if (UTIL_TABLE_IS_SUPER_TABLE(pTableMetaInfo)) {  // super table query
    if (getColumnIndexByName(&columnName, pQueryInfo, &index, pMsgBuf) != TSDB_CODE_SUCCESS) {
      return buildInvalidOperationMsg(pMsgBuf, msg1);
    }

    bool orderByTags = false;
    bool orderByTS = false;
    bool orderByGroupbyCol = false;

    if (TSDB_COL_IS_TAG(index.type) && index.columnIndex != TSDB_TBNAME_COLUMN_INDEX) {
      // it is a tag column
      if (pQueryInfo->groupbyExpr.columnInfo == NULL) {
        return buildInvalidOperationMsg(pMsgBuf, msg4);
      }

      int32_t relTagIndex = index.columnIndex - numOfCols;
      SColIndex* pColIndex = taosArrayGet(pQueryInfo->groupbyExpr.columnInfo, 0);
      if (relTagIndex == pColIndex->colIndex) {
        orderByTags = true;
      }
    } else if (index.columnIndex == TSDB_TBNAME_COLUMN_INDEX) {
      orderByTags = true;
    }

    if (PRIMARYKEY_TIMESTAMP_COL_ID == index.columnIndex) {
      orderByTS = true;
    }

    SArray *columnInfo = pQueryInfo->groupbyExpr.columnInfo;
    if (columnInfo != NULL && taosArrayGetSize(columnInfo) > 0) {
      SColIndex* pColIndex = taosArrayGet(columnInfo, 0);
      if (PRIMARYKEY_TIMESTAMP_COL_ID != index.columnIndex && pColIndex->colIndex == index.columnIndex) {
        orderByGroupbyCol = true;
      }
    }

    if (!(orderByTags || orderByTS || orderByGroupbyCol) /*&& !isTopBottomQuery(pQueryInfo)*/) {
      return buildInvalidOperationMsg(pMsgBuf, msg3);
    } else {  // order by top/bottom result value column is not supported in case of interval query.
      assert(!(orderByTags && orderByTS && orderByGroupbyCol));
    }

    size_t s = taosArrayGetSize(pSortOrder);
    if (s == 1) {
      if (orderByTags) {
        pQueryInfo->groupbyExpr.orderIndex = index.columnIndex - numOfCols;

        SListItem* p1 = taosArrayGet(pSqlNode->pSortOrder, 0);
        pQueryInfo->groupbyExpr.orderType = p1->sortOrder;
      } else if (orderByGroupbyCol) {
        SListItem* p1 = taosArrayGet(pSqlNode->pSortOrder, 0);

        pQueryInfo->groupbyExpr.orderType = p1->sortOrder;
        pQueryInfo->order.orderColId = pSchema[index.columnIndex].colId;
      } else if (isTopBottomQuery(pQueryInfo)) {
        /* order of top/bottom query in interval is not valid  */
        int32_t pos = tscExprTopBottomIndex(pQueryInfo);
        assert(pos > 0);
        
        SExprInfo* pExpr = getExprInfo(pQueryInfo, pos - 1);
//        assert(getExprFunctionId(pExpr) == FUNCTION_TS);

        pExpr = getExprInfo(pQueryInfo, pos);

        // other tag are not allowed
        if (pExpr->base.colInfo.colIndex != index.columnIndex && index.columnIndex != PRIMARYKEY_TIMESTAMP_COL_ID) {
          return buildInvalidOperationMsg(pMsgBuf, msg5);
        }

        SListItem* p1 = taosArrayGet(pSqlNode->pSortOrder, 0);
        pQueryInfo->order.order = p1->sortOrder;
        pQueryInfo->order.orderColId = pSchema[index.columnIndex].colId;
        return TSDB_CODE_SUCCESS;
      } else {
        SListItem* p1 = taosArrayGet(pSqlNode->pSortOrder, 0);

        pQueryInfo->order.order = p1->sortOrder;
        pQueryInfo->order.orderColId = PRIMARYKEY_TIMESTAMP_COL_ID;

        // orderby ts query on super table
        if (tscOrderedProjectionQueryOnSTable(pQueryInfo, 0)) {
          bool found = false;
          for (int32_t i = 0; i < getNumOfExprs(pQueryInfo); ++i) {
            SExprInfo* pExpr = getExprInfo(pQueryInfo, i);
            if (getExprFunctionId(pExpr) == FUNCTION_PRJ && pExpr->base.colInfo.colId == PRIMARYKEY_TIMESTAMP_COL_ID) {
              found = true;
              break;
            }
          }

          if (!found && pQueryInfo->pDownstream) {
            return buildInvalidOperationMsg(pMsgBuf, msg9);
          }

          // this is a invisible output column, in order to used to sort the result.
          setTsOutputExprInfo(pQueryInfo, pTableMetaInfo, 0, index.tableIndex);
        }
      }
    } else {
      SListItem *pItem = taosArrayGet(pSqlNode->pSortOrder, 0);
      if (orderByTags) {
        pQueryInfo->groupbyExpr.orderIndex = index.columnIndex - numOfCols;
        pQueryInfo->groupbyExpr.orderType = pItem->sortOrder;
      } else if (orderByGroupbyCol) {
        pQueryInfo->order.order = pItem->sortOrder;
        pQueryInfo->order.orderColId = index.columnIndex;
      } else {
        pQueryInfo->order.order = pItem->sortOrder;
        pQueryInfo->order.orderColId = PRIMARYKEY_TIMESTAMP_COL_ID;
      }

      pItem = taosArrayGet(pSqlNode->pSortOrder, 1);
      SVariant* pVar2 = &pItem->pVar;
      SToken cname = {pVar2->nLen, pVar2->nType, pVar2->pz};
      if (getColumnIndexByName(&cname, pQueryInfo, &index, pMsgBuf) != TSDB_CODE_SUCCESS) {
        return buildInvalidOperationMsg(pMsgBuf, msg1);
      }

      if (index.columnIndex != PRIMARYKEY_TIMESTAMP_COL_ID) {
        return buildInvalidOperationMsg(pMsgBuf, msg6);
      } else {
        SListItem* p1 = taosArrayGet(pSortOrder, 1);
        pQueryInfo->order.order = p1->sortOrder;
        pQueryInfo->order.orderColId = PRIMARYKEY_TIMESTAMP_COL_ID;
      }
    }

  } else if (UTIL_TABLE_IS_NORMAL_TABLE(pTableMetaInfo) || UTIL_TABLE_IS_CHILD_TABLE(pTableMetaInfo)) { // check order by clause for normal table & temp table
    if (getColumnIndexByName(&columnName, pQueryInfo, &index, pMsgBuf) != TSDB_CODE_SUCCESS) {
      return buildInvalidOperationMsg(pMsgBuf, msg1);
    }

    if (index.columnIndex != PRIMARYKEY_TIMESTAMP_COL_ID && !isTopBottomQuery(pQueryInfo)) {
      bool validOrder = false;
      SArray *columnInfo = pQueryInfo->groupbyExpr.columnInfo;
      if (columnInfo != NULL && taosArrayGetSize(columnInfo) > 0) {
        SColIndex* pColIndex = taosArrayGet(columnInfo, 0);
        validOrder = (pColIndex->colIndex == index.columnIndex);
      }

      if (!validOrder) {
        return buildInvalidOperationMsg(pMsgBuf, msg7);
      }

      SListItem* p1 = taosArrayGet(pSqlNode->pSortOrder, 0);
      pQueryInfo->groupbyExpr.orderIndex = pSchema[index.columnIndex].colId;
      pQueryInfo->groupbyExpr.orderType = p1->sortOrder;
    }

    if (isTopBottomQuery(pQueryInfo)) {
      SArray *columnInfo = pQueryInfo->groupbyExpr.columnInfo;
      if (columnInfo != NULL && taosArrayGetSize(columnInfo) > 0) {
        SColIndex* pColIndex = taosArrayGet(columnInfo, 0);

        if (pColIndex->colIndex == index.columnIndex) {
          return buildInvalidOperationMsg(pMsgBuf, msg8);
        }
      } else {
        int32_t pos = tscExprTopBottomIndex(pQueryInfo);
        assert(pos > 0);
        SExprInfo* pExpr = getExprInfo(pQueryInfo, pos - 1);
        assert(getExprFunctionId(pExpr) == FUNCTION_TS);

        pExpr = getExprInfo(pQueryInfo, pos);

        if (pExpr->base.colInfo.colIndex != index.columnIndex && index.columnIndex != PRIMARYKEY_TIMESTAMP_COL_ID) {
          return buildInvalidOperationMsg(pMsgBuf, msg5);
        }
      }

      SListItem* pItem = taosArrayGet(pSqlNode->pSortOrder, 0);
      pQueryInfo->order.order = pItem->sortOrder;

      pQueryInfo->order.orderColId = pSchema[index.columnIndex].colId;
      return TSDB_CODE_SUCCESS;
    }

    SListItem* pItem = taosArrayGet(pSqlNode->pSortOrder, 0);
    pQueryInfo->order.order = pItem->sortOrder;
    pQueryInfo->order.orderColId = pSchema[index.columnIndex].colId;
  } else {
    // handle the temp table order by clause. You can order by any single column in case of the temp table, created by
    // inner subquery.
    assert(UTIL_TABLE_IS_TMP_TABLE(pTableMetaInfo) && taosArrayGetSize(pSqlNode->pSortOrder) == 1);

    if (getColumnIndexByName(&columnName, pQueryInfo, &index, pMsgBuf) != TSDB_CODE_SUCCESS) {
      return buildInvalidOperationMsg(pMsgBuf, msg1);
    }

    SListItem* pItem = taosArrayGet(pSqlNode->pSortOrder, 0);
    pQueryInfo->order.order = pItem->sortOrder;
    pQueryInfo->order.orderColId = pSchema[index.columnIndex].colId;
  }

  return TSDB_CODE_SUCCESS;
}
#endif

static int32_t checkFillQueryRange(SQueryStmtInfo* pQueryInfo, SMsgBuf* pMsgBuf) {
  const char* msg3 = "start(end) time of time range required or time range too large";

  if (pQueryInfo->interval.interval == 0) {
    return TSDB_CODE_SUCCESS;
  }

  bool initialWindows = TSWINDOW_IS_EQUAL(pQueryInfo->window, TSWINDOW_INITIALIZER);
  if (initialWindows) {
    return buildInvalidOperationMsg(pMsgBuf, msg3);
  }

  int64_t timeRange = ABS(pQueryInfo->window.skey - pQueryInfo->window.ekey);

  int64_t intervalRange = 0;
  if (!TIME_IS_VAR_DURATION(pQueryInfo->interval.intervalUnit)) {
    intervalRange = pQueryInfo->interval.interval;

    // number of result is not greater than 10,000,000
    if ((timeRange == 0) || (timeRange / intervalRange) >= MAX_INTERVAL_TIME_WINDOW) {
      return buildInvalidOperationMsg(pMsgBuf, msg3);
    }
  }

  return TSDB_CODE_SUCCESS;
}

int32_t validateFillNode(SQueryStmtInfo *pQueryInfo, SSqlNode* pSqlNode, SMsgBuf* pMsgBuf) {
  SArray* pFillToken = pSqlNode->fillType;
  if (pSqlNode->fillType == NULL) {
    return TSDB_CODE_SUCCESS;
  }

  SListItem* pItem = taosArrayGet(pFillToken, 0);

  const int32_t START_INTERPO_COL_IDX = 1;

  const char* msg1 = "value is expected";
  const char* msg2 = "invalid fill option";
  const char* msg4 = "illegal value or data overflow";
  const char* msg6 = "not supported function now";

  /*
   * fill options are set at the end position, when all columns are set properly
   * the columns may be increased due to group by operation
   */
  if (checkFillQueryRange(pQueryInfo, pMsgBuf) != TSDB_CODE_SUCCESS) {
    return TSDB_CODE_TSC_INVALID_OPERATION;
  }


  if (pItem->pVar.nType != TSDB_DATA_TYPE_BINARY) {
    return buildInvalidOperationMsg(pMsgBuf, msg2);
  }

  int32_t numOfFields = (int32_t) getNumOfFields(&pQueryInfo->fieldsInfo);

  pQueryInfo->fillVal = calloc(numOfFields, sizeof(int64_t));
  if (pQueryInfo->fillVal == NULL) {
    return TSDB_CODE_TSC_OUT_OF_MEMORY;
  }

  pQueryInfo->numOfFillVal = (int32_t)numOfFields;
  if (strncasecmp(pItem->pVar.pz, "none", 4) == 0 && pItem->pVar.nLen == 4) {
    pQueryInfo->fillType = TSDB_FILL_NONE;
  } else if (strncasecmp(pItem->pVar.pz, "null", 4) == 0 && pItem->pVar.nLen == 4) {
    pQueryInfo->fillType = TSDB_FILL_NULL;
    for (int32_t i = START_INTERPO_COL_IDX; i < numOfFields; ++i) {
      TAOS_FIELD* pField = &getInternalField(&pQueryInfo->fieldsInfo, i)->field;
      setNull((char*)&pQueryInfo->fillVal[i], pField->type, pField->bytes);
    }
  } else if (strncasecmp(pItem->pVar.pz, "prev", 4) == 0 && pItem->pVar.nLen == 4) {
    pQueryInfo->fillType = TSDB_FILL_PREV;
//    if (pQueryInfo->info.interpQuery && pQueryInfo->order.order == TSDB_ORDER_DESC) {
//      return buildInvalidOperationMsg(pMsgBuf, msg6);
//    }
  } else if (strncasecmp(pItem->pVar.pz, "next", 4) == 0 && pItem->pVar.nLen == 4) {
    pQueryInfo->fillType = TSDB_FILL_NEXT;
  } else if (strncasecmp(pItem->pVar.pz, "linear", 6) == 0 && pItem->pVar.nLen == 6) {
    pQueryInfo->fillType = TSDB_FILL_LINEAR;
  } else if (strncasecmp(pItem->pVar.pz, "value", 5) == 0 && pItem->pVar.nLen == 5) {
    pQueryInfo->fillType = TSDB_FILL_SET_VALUE;

    size_t num = taosArrayGetSize(pFillToken);
    if (num == 1) {  // no actual value, return with error code
      return buildInvalidOperationMsg(pMsgBuf, msg1);
    }

    int32_t startPos = 1;
    int32_t numOfFillVal = (int32_t)(num - 1);

    // for point interpolation query, we do not have the timestamp column
    if (pQueryInfo->info.interpQuery) {
      startPos = 0;
      if (numOfFillVal > numOfFields) {
        numOfFillVal = numOfFields;
      }
    } else {
      numOfFillVal = MIN(num, numOfFields);
    }

    int32_t j = 1;

    for (int32_t i = startPos; i < numOfFillVal; ++i, ++j) {
      TAOS_FIELD* pField = &getInternalField(&pQueryInfo->fieldsInfo, i)->field;
      if (pField->type == TSDB_DATA_TYPE_BINARY || pField->type == TSDB_DATA_TYPE_NCHAR) {
        setVardataNull((char*) &pQueryInfo->fillVal[i], pField->type);
        continue;
      }

      SVariant* p = taosArrayGet(pFillToken, j);
      int32_t ret = taosVariantDump(p, (char*)&pQueryInfo->fillVal[i], pField->type, true);
      if (ret != TSDB_CODE_SUCCESS) {
        return buildInvalidOperationMsg(pMsgBuf, msg4);
      }
    }

    if ((num < numOfFields) || ((num - 1 < numOfFields) && (pQueryInfo->info.interpQuery))) {
      SListItem* lastItem = taosArrayGetLast(pFillToken);

      for (int32_t i = numOfFillVal; i < numOfFields; ++i) {
        TAOS_FIELD* pField = &getInternalField(&pQueryInfo->fieldsInfo, i)->field;

        if (pField->type == TSDB_DATA_TYPE_BINARY || pField->type == TSDB_DATA_TYPE_NCHAR) {
          setVardataNull((char*) &pQueryInfo->fillVal[i], pField->type);
        } else {
          taosVariantDump(&lastItem->pVar, (char*)&pQueryInfo->fillVal[i], pField->type, true);
        }
      }
    }
  } else {
    return buildInvalidOperationMsg(pMsgBuf, msg2);
  }

  return TSDB_CODE_SUCCESS;
}

int32_t validateSqlNode(SSqlNode* pSqlNode, SQueryStmtInfo* pQueryInfo, SMsgBuf* pMsgBuf) {
  assert(pSqlNode != NULL && (pSqlNode->from == NULL || taosArrayGetSize(pSqlNode->from->list) > 0));

  const char* msg1 = "point interpolation query needs timestamp";
  const char* msg2 = "too many tables in from clause";
  const char* msg3 = "start(end) time of query range required or time range too large";
  const char* msg4 = "interval query not supported, since the result of sub query not include valid timestamp column";
  const char* msg5 = "only tag query not compatible with normal column filter";
  const char* msg7 = "derivative/twa/irate requires timestamp column exists in subquery";
  const char* msg8 = "condition missing for join query";

  int32_t  code = TSDB_CODE_SUCCESS;

  /*
   * handle the sql expression without from subclause
   * select server_status();
   * select server_version();
   * select client_version();
   * select database();
   * select 1+2;
   * select now();
   */
  if (pSqlNode->from == NULL) {
    assert(pSqlNode->fillType == NULL && pSqlNode->pGroupby == NULL && pSqlNode->pWhere == NULL &&
           pSqlNode->pSortOrder == NULL);
    assert(0);
//    return doLocalQueryProcess(pCmd, pQueryInfo, pSqlNode);
  }

  if (pSqlNode->from->type == SQL_NODE_FROM_SUBQUERY) {
    pQueryInfo->numOfTables = 0;

    // parse the subquery in the first place
    int32_t numOfSub = (int32_t)taosArrayGetSize(pSqlNode->from->list);
    for (int32_t i = 0; i < numOfSub; ++i) {
      SRelElementPair* subInfo = taosArrayGet(pSqlNode->from->list, i);
      code = doValidateSubquery(pSqlNode, i, pQueryInfo, pMsgBuf);
      if (code != TSDB_CODE_SUCCESS) {
        return code;
      }
    }

    // parse the group by clause in the first place
    if (validateGroupbyNode(pQueryInfo, pSqlNode->pGroupby, pMsgBuf) != TSDB_CODE_SUCCESS) {
      return TSDB_CODE_TSC_INVALID_OPERATION;
    }

    if (validateSelectNodeList(pQueryInfo, pSqlNode->pSelNodeList, true, pMsgBuf) != TSDB_CODE_SUCCESS) {
      return TSDB_CODE_TSC_INVALID_OPERATION;
    }

    code = checkForUnsupportedQuery(pQueryInfo, pMsgBuf);

    STableMeta* pTableMeta = getMetaInfo(pQueryInfo, 0)->pTableMeta;
    SSchema*    pSchema = getOneColumnSchema(pTableMeta, 0);
    int32_t precision = pTableMeta->tableInfo.precision;

#if 0
    if (pSchema->type != TSDB_DATA_TYPE_TIMESTAMP) {
      int32_t numOfExprs = (int32_t)getNumOfExprs(pQueryInfo);

      for (int32_t i = 0; i < numOfExprs; ++i) {
        SExprInfo* pExpr = getExprInfo(pQueryInfo, i);

        int32_t f = pExpr->pExpr->_node.functionId;
        if (f == FUNCTION_DERIVATIVE || f == FUNCTION_TWA || f == FUNCTION_IRATE) {
          return buildInvalidOperationMsg(pMsgBuf, msg7);
        }
      }
    }
#endif

    // validate the query filter condition info
    if (pSqlNode->pWhere != NULL) {
      if (validateWhereNode(pQueryInfo, pSqlNode->pWhere, pMsgBuf) != TSDB_CODE_SUCCESS) {
        return TSDB_CODE_TSC_INVALID_OPERATION;
      }
    } else {
      if (pQueryInfo->numOfTables > 1) {
        return buildInvalidOperationMsg(pMsgBuf, msg8);
      }
    }

    // validate the interval info
    if (validateIntervalNode(pQueryInfo, pSqlNode, pMsgBuf) != TSDB_CODE_SUCCESS) {
      return TSDB_CODE_TSC_INVALID_OPERATION;
    } else {
      if (validateSessionNode(pQueryInfo, &pSqlNode->sessionVal, precision, pMsgBuf) != TSDB_CODE_SUCCESS) {
        return TSDB_CODE_TSC_INVALID_OPERATION;
      }

      // parse the window_state
      if (validateStateWindowNode(pQueryInfo, &pSqlNode->windowstateVal, pMsgBuf) != TSDB_CODE_SUCCESS) {
        return TSDB_CODE_TSC_INVALID_OPERATION;
      }
    }

    // parse the having clause in the first place
    int32_t joinQuery = (pSqlNode->from != NULL && taosArrayGetSize(pSqlNode->from->list) > 1);
    if (validateHavingNode(pQueryInfo, pSqlNode, pMsgBuf) != TSDB_CODE_SUCCESS) {
      return TSDB_CODE_TSC_INVALID_OPERATION;
    }

    if ((code = validateLimitNode(pQueryInfo, pSqlNode, pMsgBuf)) != TSDB_CODE_SUCCESS) {
      return code;
    }

    // set order by info
    if (validateOrderbyNode(pQueryInfo, pSqlNode, pMsgBuf) != TSDB_CODE_SUCCESS) {
      return TSDB_CODE_TSC_INVALID_OPERATION;
    }

    if ((code = validateFillNode(pQueryInfo, pSqlNode, pMsgBuf)) != TSDB_CODE_SUCCESS) {
      return code;
    }
  } else {
    pQueryInfo->command = TSDB_SQL_SELECT;
    if (taosArrayGetSize(pSqlNode->from->list) > TSDB_MAX_JOIN_TABLE_NUM) {
      return buildInvalidOperationMsg(pMsgBuf, msg2);
    }

    STableMetaInfo* pTableMetaInfo = getMetaInfo(pQueryInfo, 0);
    pQueryInfo->info.stableQuery = UTIL_TABLE_IS_SUPER_TABLE(pTableMetaInfo);

    int32_t precision = pTableMetaInfo->pTableMeta->tableInfo.precision;

    // parse the group by clause in the first place
    if (validateGroupbyNode(pQueryInfo, pSqlNode->pGroupby, pMsgBuf) != TSDB_CODE_SUCCESS) {
      return TSDB_CODE_TSC_INVALID_OPERATION;
    }

    // set where info
    if (pSqlNode->pWhere != NULL) {
      if (validateWhereNode(pQueryInfo, pSqlNode->pWhere, pMsgBuf) != TSDB_CODE_SUCCESS) {
        return TSDB_CODE_TSC_INVALID_OPERATION;
      }
    } else {
      if (taosArrayGetSize(pSqlNode->from->list) > 1) { // Cross join not allowed yet
        return buildInvalidOperationMsg(pMsgBuf, "cross join not supported yet");
      }
    }

    if (validateSelectNodeList(pQueryInfo, pSqlNode->pSelNodeList, false, pMsgBuf) != TSDB_CODE_SUCCESS) {
      return TSDB_CODE_TSC_INVALID_OPERATION;
    }

    // parse the window_state
    if (validateStateWindowNode(pQueryInfo, &pSqlNode->windowstateVal, pMsgBuf) != TSDB_CODE_SUCCESS) {
      return TSDB_CODE_TSC_INVALID_OPERATION;
    }

    // set order by info
    if (validateOrderbyNode(pQueryInfo, pSqlNode, pMsgBuf) != TSDB_CODE_SUCCESS) {
      return TSDB_CODE_TSC_INVALID_OPERATION;
    }

    // set interval value
    if (validateIntervalNode(pQueryInfo, pSqlNode, pMsgBuf) != TSDB_CODE_SUCCESS) {
      return TSDB_CODE_TSC_INVALID_OPERATION;
    }

    // parse the having clause in the first place
    if (validateHavingNode(pQueryInfo, pSqlNode, pMsgBuf) != TSDB_CODE_SUCCESS) {
      return TSDB_CODE_TSC_INVALID_OPERATION;
    }

    /*
     * transfer sql functions that need secondary merge into another format
     * in dealing with super table queries such as: count/first/last
     */
    if (validateSessionNode(pQueryInfo, &pSqlNode->sessionVal, precision, pMsgBuf) != TSDB_CODE_SUCCESS) {
      return TSDB_CODE_TSC_INVALID_OPERATION;
    }

    // no result due to invalid query time range
    if (pQueryInfo->window.skey > pQueryInfo->window.ekey) {
      pQueryInfo->command = TSDB_SQL_RETRIEVE_EMPTY_RESULT;
      return TSDB_CODE_SUCCESS;
    }

    if ((code = validateLimitNode(pQueryInfo, pSqlNode, pMsgBuf)) != TSDB_CODE_SUCCESS) {
      return code;
    }

    if ((code = validateFillNode(pQueryInfo, pSqlNode, pMsgBuf)) != TSDB_CODE_SUCCESS) {
      return code;
    }
  }

  return TSDB_CODE_SUCCESS;  // Does not build query message here
}

int32_t checkForInvalidExpr(SQueryStmtInfo* pQueryInfo, SMsgBuf* pMsgBuf) {
  assert(pQueryInfo != NULL && pMsgBuf != NULL);

  const char* msg1 = "invalid query expression";
  const char* msg2 = "top/bottom query does not support order by value in time window query";
  const char* msg3 = "fill only available in time window query";
  const char* msg4 = "top/bottom not support fill";
  const char* msg5 = "scalar function can not be used in time window query";
  const char* msg6 = "not support distinct mixed with join";
  const char* msg7 = "not support distinct mixed with groupby";
  const char* msg8 = "_block_dist not support subquery, only support stable/table";

  if (pQueryInfo->info.topbotQuery) {

    // 1. invalid sql:
    // select top(col, k) from table_name [interval(1d)|session(ts, 1d)|statewindow(col)] order by k asc
    // order by normal column is not supported
    int32_t colId = pQueryInfo->order.orderColId;
    if (pQueryInfo->info.timewindow && colId != PRIMARYKEY_TIMESTAMP_COL_ID) {
      return buildInvalidOperationMsg(pMsgBuf, msg2);
    }

    // select top(col, k) from table_name interval(10s) fill(prev)
    // not support fill in top/bottom query.
    if (pQueryInfo->fillType != TSDB_FILL_NONE) {
      return buildInvalidOperationMsg(pMsgBuf, msg4);
    }
  }

  /*
   * 2. invalid sql:
   * select count(tbname)/count(tag1)/count(tag2) from super_table_name [interval(1d)|session(ts, 1d)|statewindow(col)];
   */
  if (pQueryInfo->info.timewindow) {
    size_t size = getNumOfExprs(pQueryInfo);
    for (int32_t i = 0; i < size; ++i) {
      SExprInfo* pExpr = getExprInfo(pQueryInfo, i);
      int32_t    functionId = getExprFunctionId(pExpr);
      if (functionId == FUNCTION_COUNT && TSDB_COL_IS_TAG(pExpr->base.colInfo.flag)) {
        return buildInvalidOperationMsg(pMsgBuf, msg1);
      }
    }
  }

  /*
   * 3. invalid sql:
   * select tbname, tags_fields from super_table_name [interval(1s)|session(ts,1s)|statewindow(col)]
   */
  if (pQueryInfo->info.onlyTagQuery && pQueryInfo->info.timewindow) {
    return buildInvalidOperationMsg(pMsgBuf, msg1);
  }

  /*
   * 4. invalid sql:
   * select * from table_name fill(prev|next|null|none)
   */
  if (!pQueryInfo->info.timewindow && !pQueryInfo->info.interpQuery && pQueryInfo->fillType != TSDB_FILL_NONE) {
    return buildInvalidOperationMsg(pMsgBuf, msg3);
  }

  /*
   * 5. invalid sql:
   * select diff(col)|derivative(col)|* from table_name interval(1s)|session(20s)|statewindow(col)
   * projection query not compatible with the time window query
   */
  if (pQueryInfo->info.timewindow && pQueryInfo->info.projectionQuery) {
    return buildInvalidOperationMsg(pMsgBuf, msg5);
  }

  /*
   * 6. invalid sql:
   * distinct + join not supported.
   * select distinct a,b from table1, table2 where table1.ts=table2.ts
   *
   * distinct + group by not supported:
   * select distinct count(a) from table_name group by col1;
   */
  if (pQueryInfo->info.distinct) {
    if (pQueryInfo->info.join) {
      return buildInvalidOperationMsg(pMsgBuf, msg6);
    }

    if (taosArrayGetSize(pQueryInfo->groupbyExpr.columnInfo) != 0) {
      return buildInvalidOperationMsg(pMsgBuf, msg7);
    }
  }

  /*
   * 7. invalid sql:
   * nested subquery not support block_dist query
   * select block_dist() from (select * from table_name)
   */
}

static int32_t resColId = 5000;
int32_t getNewResColId() {
  return resColId++;
}

int32_t addResColumnInfo(SQueryStmtInfo* pQueryInfo, int32_t outputIndex, SSchema* pSchema, SExprInfo* pSqlExpr) {
  SInternalField* pInfo = insertFieldInfo(&pQueryInfo->fieldsInfo, outputIndex, pSchema);
  pInfo->pExpr = pSqlExpr;
  return TSDB_CODE_SUCCESS;
}

void setResultColName(char* name, tSqlExprItem* pItem, SToken* pToken, SToken* functionToken, bool multiCols) {
  if (pItem->aliasName != NULL) {
    tstrncpy(name, pItem->aliasName, TSDB_COL_NAME_LEN);
  } else if (multiCols) {
    char uname[TSDB_COL_NAME_LEN] = {0};
    int32_t len = MIN(pToken->n + 1, TSDB_COL_NAME_LEN);
    tstrncpy(uname, pToken->z, len);

    if (tsKeepOriginalColumnName) { // keep the original column name
      tstrncpy(name, uname, TSDB_COL_NAME_LEN);
    } else {
      const int32_t size = TSDB_COL_NAME_LEN + FUNCTIONS_NAME_MAX_LENGTH + 2 + 1;
      char tmp[TSDB_COL_NAME_LEN + FUNCTIONS_NAME_MAX_LENGTH + 2 + 1] = {0};

      char f[FUNCTIONS_NAME_MAX_LENGTH] = {0};
      strncpy(f, functionToken->z, functionToken->n);

      snprintf(tmp, size, "%s(%s)", f, uname);
      tstrncpy(name, tmp, TSDB_COL_NAME_LEN);
    }
  } else  { // use the user-input result column name
    int32_t len = MIN(pItem->pNode->exprToken.n + 1, TSDB_COL_NAME_LEN);
    tstrncpy(name, pItem->pNode->exprToken.z, len);
  }
}

SExprInfo* doAddOneExprInfo(SQueryStmtInfo* pQueryInfo, int32_t outputColIndex, int16_t functionId, SColumnIndex* pIndex,
                           SSchema* pColSchema, SSchema* pResultSchema, tExprNode* pExprNode, int32_t interSize, const char* token, bool finalResult) {
  STableMetaInfo* pTableMetaInfo = getMetaInfo(pQueryInfo, pIndex->tableIndex);

  SExprInfo* pExpr = createExprInfo(pTableMetaInfo, functionId, pIndex, pExprNode, pResultSchema, interSize);
  addExprInfo(pQueryInfo, outputColIndex, pExpr);

  tstrncpy(pExpr->base.token, token, sizeof(pExpr->base.token));
  uint64_t uid = pTableMetaInfo->pTableMeta->uid;

  SArray* p = TSDB_COL_IS_TAG(pIndex->type)?pTableMetaInfo->tagColList:pQueryInfo->colList;
  columnListInsert(p, pIndex->columnIndex, uid, pColSchema);

  pExpr->base.colInfo.flag = pIndex->type;
  if (TSDB_COL_IS_NORMAL_COL(pIndex->type)) {
    insertPrimaryTsColumn(pQueryInfo->colList, uid);
  }

  if (finalResult) {
    addResColumnInfo(pQueryInfo, outputColIndex, pColSchema, pExpr);
  }

  return pExpr;
}

static int32_t addOneExprInfo(SQueryStmtInfo* pQueryInfo, tSqlExprItem* pItem, int32_t functionId, int32_t outputIndex, SSchema* pSchema, SColumnIndex* pColIndex, tExprNode* pNode, bool finalResult, SMsgBuf* pMsgBuf) {
  const char* msg1 = "not support column types";
  if (functionId == FUNCTION_SPREAD) {
    if (IS_VAR_DATA_TYPE(pSchema->type) || pSchema->type == TSDB_DATA_TYPE_BOOL) {
      return buildInvalidOperationMsg(pMsgBuf, msg1);
    }
  }

  char name[TSDB_COL_NAME_LEN] = {0};
  SToken t = {.z = pSchema->name, .n = (uint32_t)strnlen(pSchema->name, TSDB_COL_NAME_LEN)};
  setResultColName(name, pItem, &t, &pItem->pNode->Expr.operand, true);

  SResultDataInfo resInfo = {0};
  getResultDataInfo(pSchema->type, pSchema->bytes, functionId, 0, &resInfo, 0, false);

  SSchema resultSchema = createSchema(resInfo.type, resInfo.bytes, getNewResColId(), name);
  doAddOneExprInfo(pQueryInfo, outputIndex, functionId, pColIndex, pSchema, &resultSchema, pNode, resInfo.intermediateBytes, name, finalResult);
  return TSDB_CODE_SUCCESS;
}

static int32_t checkForAliasName(SMsgBuf* pMsgBuf, char* aliasName) {
  const char* msg1 = "column alias name too long";
  if (aliasName != NULL && strlen(aliasName) >= TSDB_COL_NAME_LEN) {
    return buildInvalidOperationMsg(pMsgBuf, msg1);
  }

  return TSDB_CODE_SUCCESS;
}

static int32_t validateComplexExpr(tSqlExpr* pExpr, SQueryStmtInfo* pQueryInfo, SArray* pColList, int32_t* type, SMsgBuf* pMsgBuf);
static int32_t sqlExprToExprNode(tExprNode **pExpr, const tSqlExpr* pSqlExpr, SQueryStmtInfo* pQueryInfo, SArray* pCols, uint64_t *uid, SMsgBuf* pMsgBuf);

static int64_t getTickPerSecond(SVariant* pVariant, int32_t precision, int64_t* tickPerSec, SMsgBuf *pMsgBuf) {
  const char* msg10 = "derivative duration should be greater than 1 Second";

  if (taosVariantDump(pVariant, (char*) tickPerSec, TSDB_DATA_TYPE_BIGINT, true) < 0) {
    return TSDB_CODE_TSC_INVALID_OPERATION;
  }

  if (precision == TSDB_TIME_PRECISION_MILLI) {
    *tickPerSec /= TSDB_TICK_PER_SECOND(TSDB_TIME_PRECISION_MICRO);
  } else if (precision == TSDB_TIME_PRECISION_MICRO) {
    *tickPerSec /= TSDB_TICK_PER_SECOND(TSDB_TIME_PRECISION_MILLI);
  }

  if (*tickPerSec <= 0 || *tickPerSec < TSDB_TICK_PER_SECOND(precision)) {
    return buildInvalidOperationMsg(pMsgBuf, msg10);
  }

  return TSDB_CODE_SUCCESS;
}

// set the first column ts for top/bottom query
static void setTsOutputExprInfo(SQueryStmtInfo* pQueryInfo, STableMetaInfo* pTableMetaInfo, int32_t outputIndex, int32_t tableIndex) {
  SColumnIndex indexTS = {.tableIndex = tableIndex, .columnIndex = PRIMARYKEY_TIMESTAMP_COL_ID, .type = TSDB_COL_NORMAL};
  SSchema s = createSchema(TSDB_DATA_TYPE_TIMESTAMP, TSDB_KEYSIZE, getNewResColId(), "ts");

  SExprInfo* pExpr = createExprInfo(pTableMetaInfo, FUNCTION_TS_DUMMY, &indexTS, NULL, &s, TSDB_KEYSIZE);
  strncpy(pExpr->base.token, "ts", tListLen(pExpr->base.token));

  addExprInfo(pQueryInfo, outputIndex, pExpr);

  SSchema* pSourceSchema = getOneColumnSchema(pTableMetaInfo->pTableMeta, indexTS.columnIndex);
  columnListInsert(pQueryInfo->colList, indexTS.columnIndex, pTableMetaInfo->pTableMeta->uid, pSourceSchema);
  addResColumnInfo(pQueryInfo, outputIndex, &pExpr->base.resSchema, pExpr);
}

static int32_t setColumnIndex(SQueryStmtInfo* pQueryInfo, SArray* pParamList, SColumnIndex* index, SSchema* columnSchema, tExprNode** pNode, SMsgBuf* pMsgBuf) {
  const char* msg1 = "illegal column name";
  const char* msg2 = "invalid table name";

  STableMeta* pTableMeta = getMetaInfo(pQueryInfo, 0)->pTableMeta;
  if (pParamList == NULL) {
    // count(*) is equalled to count(primary_timestamp_key)
    *index = (SColumnIndex) {0, PRIMARYKEY_TIMESTAMP_COL_ID, false};
    *columnSchema = *(SSchema*) getOneColumnSchema(pTableMeta, index->columnIndex);
  } else {
    tSqlExprItem* pParamElem = taosArrayGet(pParamList, 0);

    SToken* pToken = &pParamElem->pNode->columnName;
    int16_t tokenId = pParamElem->pNode->tokenId;

    // select count(table.*), select count(1), count(2)
    if (tokenId == TK_ALL || tokenId == TK_INTEGER || tokenId == TK_FLOAT) {
      // check if the table name is valid or not
      SToken tmpToken = pParamElem->pNode->columnName;
      if (getTableIndexByName(&tmpToken, pQueryInfo, index) != TSDB_CODE_SUCCESS) {
        return buildInvalidOperationMsg(pMsgBuf, msg2);
      }

      *index = (SColumnIndex) {0, PRIMARYKEY_TIMESTAMP_COL_ID, false};
      *columnSchema = *(SSchema*) getOneColumnSchema(pTableMeta, index->columnIndex);
    } else if (pToken->z != NULL && pToken->n > 0) {
      // count the number of table created according to the super table
      if (getColumnIndexByName(pToken, pQueryInfo, index, pMsgBuf) != TSDB_CODE_SUCCESS) {
        return buildInvalidOperationMsg(pMsgBuf, msg1);
      }

      *columnSchema = *(SSchema*) getOneColumnSchema(pTableMeta, index->columnIndex);
    } else {
      STableMetaInfo* pTableMetaInfo = NULL;
      int32_t code = extractFunctionParameterInfo(pQueryInfo, tokenId, &pTableMetaInfo, columnSchema, pNode, index, pParamElem, pMsgBuf);
      if (code != TSDB_CODE_SUCCESS) {
        return buildInvalidOperationMsg(pMsgBuf, msg1);
      }
    }
  }

  return TSDB_CODE_SUCCESS;
}

static int32_t doAddAllColumnExprInSelectClause(SQueryStmtInfo *pQueryInfo, STableMetaInfo* pTableMetaInfo, tSqlExprItem* pItem, int32_t functionId,
    int32_t tableIndex, int32_t* colIndex, bool finalResult, SMsgBuf* pMsgBuf) {
  STableMeta* pTableMeta = pTableMetaInfo->pTableMeta;
  for (int32_t i = 0; i < getNumOfColumns(pTableMeta); ++i) {
    SColumnIndex index = {.tableIndex = tableIndex, .columnIndex = i, .type = TSDB_COL_NORMAL};

    SSchema* pSchema = getOneColumnSchema(pTableMeta, i);
    if (addOneExprInfo(pQueryInfo, pItem, functionId, *colIndex, pSchema, &index, NULL, finalResult, pMsgBuf) != 0) {
      return TSDB_CODE_TSC_INVALID_OPERATION;
    }

    (*colIndex)++;
  }
}

static int32_t doHandleOneParam(SQueryStmtInfo *pQueryInfo, tSqlExprItem* pItem, tSqlExprItem* pParamElem, int32_t functionId,
    int32_t* outputIndex, bool finalResult, SMsgBuf* pMsgBuf) {
  const char* msg3 = "illegal column name";
  const char* msg4 = "invalid table name";
  const char* msg6 = "functions applied to tags are not allowed";

  SColumnIndex index = COLUMN_INDEX_INITIALIZER;

  if (pParamElem->pNode->tokenId == TK_ALL) { // select table.*
    SToken tmpToken = pParamElem->pNode->columnName;

    if (getTableIndexByName(&tmpToken, pQueryInfo, &index) != TSDB_CODE_SUCCESS) {
      return buildInvalidOperationMsg(pMsgBuf, msg4);
    }

    STableMetaInfo* pTableMetaInfo = getMetaInfo(pQueryInfo, index.tableIndex);
    doAddAllColumnExprInSelectClause(pQueryInfo, pTableMetaInfo, pItem, functionId, index.tableIndex, outputIndex, finalResult, pMsgBuf);
  } else {
    tExprNode* pNode = NULL;
    int32_t tokenId = pParamElem->pNode->tokenId;
    SSchema columnSchema = {0};
    STableMetaInfo* pTableMetaInfo = {0};

    int32_t code = extractFunctionParameterInfo(pQueryInfo, tokenId, &pTableMetaInfo, &columnSchema, &pNode, &index, pParamElem, pMsgBuf);
    if (code != TSDB_CODE_SUCCESS) {
      return buildInvalidOperationMsg(pMsgBuf, msg3);
    }

    // functions can not be applied to tags
    if (TSDB_COL_IS_TAG(index.type) && (functionId == FUNCTION_INTERP || functionId == FUNCTION_SPREAD)) {
      return buildInvalidOperationMsg(pMsgBuf, msg6);
    }

    if (addOneExprInfo(pQueryInfo, pItem, functionId, (*outputIndex)++, &columnSchema, &index, pNode, finalResult, pMsgBuf) != 0) {
      return TSDB_CODE_TSC_INVALID_OPERATION;
    }
  }
}

static int32_t multiColumnListInsert(SQueryStmtInfo* pQueryInfo, SArray* pColumnList, SMsgBuf* pMsgBuf);

int32_t extractFunctionParameterInfo(SQueryStmtInfo* pQueryInfo, int32_t tokenId, STableMetaInfo** pTableMetaInfo, SSchema* columnSchema,
                              tExprNode** pNode, SColumnIndex* pIndex, tSqlExprItem* pParamElem, SMsgBuf* pMsgBuf) {
  const char* msg1 = "not support column types";
  const char* msg2 = "invalid parameters";
  const char* msg3 = "illegal column name";
  const char* msg4 = "nested function is not supported";
  const char* msg5 = "functions applied to tags are not allowed";

  if (tokenId == TK_ALL || tokenId == TK_ID) {  // simple parameter
    if ((getColumnIndexByName(&pParamElem->pNode->columnName, pQueryInfo, pIndex, pMsgBuf) != TSDB_CODE_SUCCESS)) {
      return buildInvalidOperationMsg(pMsgBuf, msg3);
    }

    // functions can not be applied to tags
    if (TSDB_COL_IS_TAG(pIndex->type)) {
      return buildInvalidOperationMsg(pMsgBuf, msg5);
    }

    *pTableMetaInfo = getMetaInfo(pQueryInfo, pIndex->tableIndex);

    // 2. check if sql function can be applied on this column data type
    *columnSchema = *(SSchema*) getOneColumnSchema((*pTableMetaInfo)->pTableMeta, pIndex->columnIndex);
  } else if (tokenId == TK_PLUS || tokenId == TK_MINUS || tokenId == TK_STAR || tokenId == TK_REM || tokenId == TK_DIVIDE || tokenId == TK_CONCAT) {
    int32_t arithmeticType = NON_ARITHMEIC_EXPR;
    SArray* pColumnList = taosArrayInit(4, sizeof(SColumnIndex));
    if (validateComplexExpr(pParamElem->pNode, pQueryInfo, pColumnList, &arithmeticType, pMsgBuf) != TSDB_CODE_SUCCESS) {
      return buildInvalidOperationMsg(pMsgBuf, msg1);
    }

    if (arithmeticType != NORMAL_ARITHMETIC) {
      return buildInvalidOperationMsg(pMsgBuf, msg4);
    }

    *pTableMetaInfo = getMetaInfo(pQueryInfo, 0);  // todo get the first table meta.
    *columnSchema = createSchema(TSDB_DATA_TYPE_DOUBLE, sizeof(double), getNewResColId(), "");

    SToken* pExprToken = &pParamElem->pNode->exprToken;
    int32_t len = MIN(TSDB_COL_NAME_LEN, pExprToken->n + 1);
    tstrncpy(columnSchema->name, pExprToken->z, len);

    SArray* colList = taosArrayInit(10, sizeof(SColIndex));
    int32_t ret = sqlExprToExprNode(pNode, pParamElem->pNode, pQueryInfo, colList, NULL, pMsgBuf);
    if (ret != TSDB_CODE_SUCCESS) {
      taosArrayDestroy(colList);
      tExprTreeDestroy(*pNode, NULL);
      return buildInvalidOperationMsg(pMsgBuf, msg2);
    }

    pIndex->tableIndex = 0;
    multiColumnListInsert(pQueryInfo, pColumnList, pMsgBuf);
    taosArrayDestroy(colList);
    taosArrayDestroy(pColumnList);
  } else {
    assert(0);
  }
  
  return TSDB_CODE_SUCCESS;
}

static int32_t checkForkParam(tSqlExpr* pSqlExpr, size_t k, SMsgBuf* pMsgBuf) {
  const char* msg1 = "invalid parameters";

  if (k == 0) {
    if (pSqlExpr->Expr.paramList != NULL && taosArrayGetSize(pSqlExpr->Expr.paramList) != 0) {
      return buildInvalidOperationMsg(pMsgBuf, msg1);
    }
  } else {
    if (pSqlExpr->Expr.paramList == NULL || taosArrayGetSize(pSqlExpr->Expr.paramList) != k) {
      return buildInvalidOperationMsg(pMsgBuf, msg1);
    }
  }
  return TSDB_CODE_SUCCESS;
}

int32_t addExprAndResColumn(SQueryStmtInfo* pQueryInfo, int32_t colIndex, tSqlExprItem* pItem, bool finalResult, SMsgBuf* pMsgBuf) {
  STableMetaInfo* pTableMetaInfo = NULL;
  int32_t functionId = pItem->functionId;
  int32_t code = TSDB_CODE_SUCCESS;

  const char* msg1 = "not support column types";
  const char* msg2 = "invalid parameters";
  const char* msg3 = "illegal column name";
  const char* msg4 = "invalid table name";
  const char* msg5 = "parameter is out of range [0, 100]";
  const char* msg6 = "functions applied to tags are not allowed";
  const char* msg7 = "normal table can not apply this function";
  const char* msg8 = "multi-columns selection does not support alias column name";
  const char* msg9 = "diff/derivative can no be applied to unsigned numeric type";
  const char* msg10 = "derivative duration should be greater than 1 Second";
  const char* msg11 = "third parameter in derivative should be 0 or 1";
  const char* msg12 = "parameter is out of range [1, 100]";
  const char* msg13 = "nested function is not supported";

  if (checkForAliasName(pMsgBuf, pItem->aliasName) != TSDB_CODE_SUCCESS) {
    return TSDB_CODE_TSC_INVALID_OPERATION;
  }

  switch (functionId) {
    case FUNCTION_COUNT: {
      // more than one parameter for count() function
      SArray* pParamList = pItem->pNode->Expr.paramList;
      if ((code = checkForkParam(pItem->pNode, 1, pMsgBuf)) != TSDB_CODE_SUCCESS) {
        return code;
      }

      tExprNode* pNode = NULL;
      SColumnIndex index = COLUMN_INDEX_INITIALIZER;
      SSchema columnSchema = {0};

      code = setColumnIndex(pQueryInfo, pParamList, &index, &columnSchema, &pNode, pMsgBuf);
      if (code != TSDB_CODE_SUCCESS) {
        return code;
      }

      int32_t size = tDataTypes[TSDB_DATA_TYPE_BIGINT].bytes;
      SSchema s = createSchema(TSDB_DATA_TYPE_BIGINT, size, getNewResColId(), "");

      char token[TSDB_COL_NAME_LEN] = {0};
      setTokenAndResColumnName(pItem, s.name, token,sizeof(s.name) - 1);

      int32_t outputIndex = getNumOfFields(&pQueryInfo->fieldsInfo);
      doAddOneExprInfo(pQueryInfo, outputIndex, functionId, &index, &columnSchema, &s, pNode, size, token, finalResult);
      return TSDB_CODE_SUCCESS;
    }

    case FUNCTION_SUM:
    case FUNCTION_AVG:
    case FUNCTION_RATE:
    case FUNCTION_IRATE:
    case FUNCTION_TWA:
    case FUNCTION_MIN:
    case FUNCTION_MAX:
    case FUNCTION_DIFF:
    case FUNCTION_DERIVATIVE:
    case FUNCTION_STDDEV:
    case FUNCTION_LEASTSQR: {
      // 1. valid the number of parameters
      int32_t numOfParams = (pItem->pNode->Expr.paramList == NULL)? 0: (int32_t) taosArrayGetSize(pItem->pNode->Expr.paramList);

      // no parameters or more than one parameter for function
      if (pItem->pNode->Expr.paramList == NULL ||
          (functionId != FUNCTION_LEASTSQR && functionId != FUNCTION_DERIVATIVE && numOfParams != 1) ||
          ((functionId == FUNCTION_LEASTSQR || functionId == FUNCTION_DERIVATIVE) && numOfParams != 3)) {
        return buildInvalidOperationMsg(pMsgBuf, msg2);
      }

      tSqlExprItem* pParamElem = taosArrayGet(pItem->pNode->Expr.paramList, 0);

      tExprNode* pNode = NULL;
      int32_t tokenId = pParamElem->pNode->tokenId;
      SColumnIndex index = COLUMN_INDEX_INITIALIZER;
      SSchema columnSchema = {0};

      code = extractFunctionParameterInfo(pQueryInfo, tokenId, &pTableMetaInfo, &columnSchema, &pNode, &index, pParamElem,pMsgBuf);
      if (code != TSDB_CODE_SUCCESS) {
        return code;
      }
      
      if (tokenId == TK_ALL || tokenId == TK_ID) {
        if (!IS_NUMERIC_TYPE(columnSchema.type)) {
          return buildInvalidOperationMsg(pMsgBuf, msg1);
        } else if (IS_UNSIGNED_NUMERIC_TYPE(columnSchema.type) && (functionId == FUNCTION_DIFF || functionId == FUNCTION_DERIVATIVE)) {
          return buildInvalidOperationMsg(pMsgBuf, msg9);
        }
      }

      int32_t precision = pTableMetaInfo->pTableMeta->tableInfo.precision;

      SResultDataInfo resInfo = {0};
      if (getResultDataInfo(columnSchema.type, columnSchema.bytes, functionId, 0, &resInfo, 0, false) != TSDB_CODE_SUCCESS) {
        return TSDB_CODE_TSC_INVALID_OPERATION;
      }

      // set the first column ts for diff query
      int32_t numOfOutput = getNumOfFields(&pQueryInfo->fieldsInfo);
      if (functionId == FUNCTION_DIFF || functionId == FUNCTION_DERIVATIVE) {
        setTsOutputExprInfo(pQueryInfo, pTableMetaInfo, numOfOutput, index.tableIndex);
        numOfOutput += 1;
      }

      SSchema s = createSchema(resInfo.type, resInfo.bytes, getNewResColId(), "ts");

      char token[TSDB_COL_NAME_LEN] = {0};
      setTokenAndResColumnName(pItem, s.name, token, sizeof(s.name) - 1);

      SExprInfo* pExpr = doAddOneExprInfo(pQueryInfo, numOfOutput, functionId, &index, &columnSchema, &s, pNode, resInfo.intermediateBytes, token, finalResult);

      if (functionId == FUNCTION_LEASTSQR) { // set the leastsquares parameters
        char val[8] = {0};
        if (taosVariantDump(&pParamElem[1].pNode->value, val, TSDB_DATA_TYPE_DOUBLE, true) < 0) {
          return TSDB_CODE_TSC_INVALID_OPERATION;
        }

        addExprInfoParam(&pExpr->base, val, TSDB_DATA_TYPE_DOUBLE, DOUBLE_BYTES);

        memset(val, 0, tListLen(val));
        if (taosVariantDump(&pParamElem[2].pNode->value, val, TSDB_DATA_TYPE_DOUBLE, true) < 0) {
          return TSDB_CODE_TSC_INVALID_OPERATION;
        }

        addExprInfoParam(&pExpr->base, val, TSDB_DATA_TYPE_DOUBLE, DOUBLE_BYTES);
      } else if (functionId == FUNCTION_IRATE) {
        addExprInfoParam(&pExpr->base, (char*) &precision, TSDB_DATA_TYPE_BIGINT, LONG_BYTES);
      } else if (functionId == FUNCTION_DERIVATIVE) {
        char val[8] = {0};

        int64_t tickPerSec = 0;
        code = getTickPerSecond(&pParamElem[1].pNode->value, precision, &tickPerSec, pMsgBuf);
        if (code != TSDB_CODE_SUCCESS) {
          return code;
        }

        addExprInfoParam(&pExpr->base, (char*) &tickPerSec, TSDB_DATA_TYPE_BIGINT, LONG_BYTES);
        memset(val, 0, tListLen(val));

        if (taosVariantDump(&pParamElem[2].pNode->value, val, TSDB_DATA_TYPE_BIGINT, true) < 0) {
          return TSDB_CODE_TSC_INVALID_OPERATION;
        }

        if (GET_INT64_VAL(val) != 0 && GET_INT64_VAL(val) != 1) {
          return buildInvalidOperationMsg(pMsgBuf, msg11);
        }

        addExprInfoParam(&pExpr->base, val, TSDB_DATA_TYPE_BIGINT, LONG_BYTES);
      }
      return TSDB_CODE_SUCCESS;
    }

    case FUNCTION_FIRST:
    case FUNCTION_LAST:
    case FUNCTION_SPREAD:
    case FUNCTION_LAST_ROW:
    case FUNCTION_INTERP: {
      bool requireAllFields = (pItem->pNode->Expr.paramList == NULL);

      if (!requireAllFields) {
        SArray* pParamList = pItem->pNode->Expr.paramList;
        if (taosArrayGetSize(pParamList) < 1) {
          return buildInvalidOperationMsg(pMsgBuf, msg3);
        }

        if (taosArrayGetSize(pParamList) > 1 && (pItem->aliasName != NULL)) {
          return buildInvalidOperationMsg(pMsgBuf, msg8);
        }

        // in first/last function, multiple columns can be add to resultset
        for (int32_t i = 0; i < taosArrayGetSize(pParamList); ++i) {
          tSqlExprItem* pParamElem = taosArrayGet(pParamList, i);
          doHandleOneParam(pQueryInfo, pItem, pParamElem, functionId, &colIndex, finalResult, pMsgBuf);
        }
      } else {  // select function(*) from xxx
        int32_t numOfFields = 0;

        // multicolumn selection does not support alias name
        if (pItem->aliasName != NULL && strlen(pItem->aliasName) > 0) {
          return buildInvalidOperationMsg(pMsgBuf, msg8);
        }

        for (int32_t j = 0; j < pQueryInfo->numOfTables; ++j) {
          pTableMetaInfo = getMetaInfo(pQueryInfo, j);
          doAddAllColumnExprInSelectClause(pQueryInfo, pTableMetaInfo, pItem, functionId, j, &colIndex, finalResult, pMsgBuf);
          numOfFields += getNumOfColumns(pTableMetaInfo->pTableMeta);
        }
      }
      return TSDB_CODE_SUCCESS;
    }

    case FUNCTION_TOP:
    case FUNCTION_BOTTOM:
    case FUNCTION_PERCT:
    case FUNCTION_APERCT: {
      // 1. valid the number of parameters
      // no parameters or more than one parameter for function
      if ((code = checkForkParam(pItem->pNode, 2, pMsgBuf)) != TSDB_CODE_SUCCESS) {
        return code;
      }

      tSqlExprItem* pParamElem = taosArrayGet(pItem->pNode->Expr.paramList, 0);
      if (pParamElem->pNode->tokenId == TK_ALL) {
        return buildInvalidOperationMsg(pMsgBuf, msg2);
      }

      tExprNode* pNode = NULL;
      int32_t tokenId = pParamElem->pNode->tokenId;
      SColumnIndex index = COLUMN_INDEX_INITIALIZER;
      SSchema columnSchema = {0};
      code = extractFunctionParameterInfo(pQueryInfo, tokenId, &pTableMetaInfo, &columnSchema, &pNode, &index, pParamElem,pMsgBuf);
      if (code != TSDB_CODE_SUCCESS) {
        return code;
      }

      // functions can not be applied to tags
      if (TSDB_COL_IS_TAG(index.type)) {
        return buildInvalidOperationMsg(pMsgBuf, msg6);
      }

      pTableMetaInfo = getMetaInfo(pQueryInfo, index.tableIndex);

      // 2. valid the column type
      if (!IS_NUMERIC_TYPE(columnSchema.type)) {
        return buildInvalidOperationMsg(pMsgBuf, msg1);
      }

      // 3. valid the parameters
      if (pParamElem[1].pNode->tokenId == TK_ID) {
        return buildInvalidOperationMsg(pMsgBuf, msg2);
      }

      SResultDataInfo resInfo = {0};
      getResultDataInfo(columnSchema.type, columnSchema.bytes, functionId, 0, &resInfo, 0, false);
      if (functionId == FUNCTION_TOP || functionId == FUNCTION_BOTTOM) {
        // set the first column ts for top/bottom query
        setTsOutputExprInfo(pQueryInfo, pTableMetaInfo, colIndex, index.tableIndex);
        colIndex += 1;  // the first column is ts
      }

      SSchema s = createSchema(resInfo.type, resInfo.bytes, getNewResColId(), "");

      char token[TSDB_COL_NAME_LEN] = {0};
      setTokenAndResColumnName(pItem, s.name, token, sizeof(s.name) - 1);
      SExprInfo* pExpr = doAddOneExprInfo(pQueryInfo, colIndex, functionId, &index, &columnSchema, &s, pNode, resInfo.intermediateBytes, token, finalResult);

      SToken* pParamToken = &pParamElem[1].pNode->exprToken;
      pExpr->base.numOfParams += 1;

      SVariant* pVar = &pExpr->base.param[0];
      if (functionId == FUNCTION_PERCT || functionId == FUNCTION_APERCT) {
        taosVariantCreate(pVar, pParamToken->z, pParamToken->n, TSDB_DATA_TYPE_DOUBLE);

        /*
         * sql function transformation
         * for dp = 0, it is actually min,
         * for dp = 100, it is max,
         */
        if (pVar->d < 0 || pVar->d > TOP_BOTTOM_QUERY_LIMIT) {
          return buildInvalidOperationMsg(pMsgBuf, msg5);
        }
      } else {
        taosVariantCreate(pVar, pParamToken->z, pParamToken->n, TSDB_DATA_TYPE_BIGINT);
        if (pVar->i <= 0 || pVar->i > 100) {  // todo use macro
          return buildInvalidOperationMsg(pMsgBuf, msg12);
        }
      }

      return TSDB_CODE_SUCCESS;
    }

    case FUNCTION_TID_TAG: {
      pTableMetaInfo = getMetaInfo(pQueryInfo, 0);
      if (UTIL_TABLE_IS_NORMAL_TABLE(pTableMetaInfo)) {
        return buildInvalidOperationMsg(pMsgBuf, msg7);
      }

      // no parameters or more than one parameter for function
      if ((code = checkForkParam(pItem->pNode, 1, pMsgBuf)) != TSDB_CODE_SUCCESS) {
        return code;
      }

      tSqlExprItem* pParamItem = taosArrayGet(pItem->pNode->Expr.paramList, 0);
      tSqlExpr* pParam = pParamItem->pNode;

      SColumnIndex index = COLUMN_INDEX_INITIALIZER;
      if (getColumnIndexByName(&pParam->columnName, pQueryInfo, &index, pMsgBuf) != TSDB_CODE_SUCCESS) {
        return buildInvalidOperationMsg(pMsgBuf, msg3);
      }

      pTableMetaInfo = getMetaInfo(pQueryInfo, index.tableIndex);
      SSchema* pSchema = getTableTagSchema(pTableMetaInfo->pTableMeta);

      // functions can not be applied to normal columns
      int32_t numOfCols = getNumOfColumns(pTableMetaInfo->pTableMeta);
      if (index.columnIndex < numOfCols && index.columnIndex != TSDB_TBNAME_COLUMN_INDEX) {
        return buildInvalidOperationMsg(pMsgBuf, msg6);
      }

      if (index.columnIndex > 0) {
        index.columnIndex -= numOfCols;
      }

      // 2. valid the column type
      int16_t colType = 0;
      if (index.columnIndex == TSDB_TBNAME_COLUMN_INDEX) {
        colType = TSDB_DATA_TYPE_BINARY;
      } else {
        colType = pSchema[index.columnIndex].type;
      }

      if (colType == TSDB_DATA_TYPE_BOOL) {
        return buildInvalidOperationMsg(pMsgBuf, msg1);
      }

      columnListInsert(pTableMetaInfo->tagColList, index.columnIndex, pTableMetaInfo->pTableMeta->uid, &pSchema[index.columnIndex]);
      SSchema* pTagSchema = getTableTagSchema(pTableMetaInfo->pTableMeta);

      SSchema s = {0};
      if (index.columnIndex == TSDB_TBNAME_COLUMN_INDEX) {
        s = *getTbnameColumnSchema();
      } else {
        s = pTagSchema[index.columnIndex];
      }

      SResultDataInfo resInfo = {0};
      int32_t ret = getResultDataInfo(s.type, s.bytes, FUNCTION_TID_TAG, 0, &resInfo, 0, 0);
      assert(ret == TSDB_CODE_SUCCESS);

      s.type  = (uint8_t)resInfo.type;
      s.bytes = resInfo.bytes;
      s.colId = getNewResColId();
      TSDB_QUERY_SET_TYPE(pQueryInfo->type, TSDB_QUERY_TYPE_TAG_FILTER_QUERY);
      
      doAddOneExprInfo(pQueryInfo, 0, FUNCTION_TID_TAG, &index, &s, &s, NULL, 0, s.name, true);
      return TSDB_CODE_SUCCESS;
    }

    case FUNCTION_BLKINFO: {
      // no parameters or more than one parameter for function
      if ((code = checkForkParam(pItem->pNode, 0, pMsgBuf))!= TSDB_CODE_SUCCESS) {
        return code;
      }

      SColumnIndex index = {.tableIndex = 0, .columnIndex = 0, .type = TSDB_COL_NORMAL};
      pTableMetaInfo = getMetaInfo(pQueryInfo, index.tableIndex);

      SResultDataInfo resInfo = {0};
      getResultDataInfo(TSDB_DATA_TYPE_INT, 4, functionId, 0, &resInfo, 0, 0);

      SSchema s = createSchema(resInfo.type, resInfo.bytes, getNewResColId(), "block_dist");
      SSchema colSchema = {0};

      char token[TSDB_COL_NAME_LEN] = {0};
      setTokenAndResColumnName(pItem, s.name, token, sizeof(s.name) - 1);
      SExprInfo* pExpr = doAddOneExprInfo(pQueryInfo, colIndex, functionId, &index, &colSchema, &s, NULL, resInfo.intermediateBytes, token, finalResult);

      int64_t rowSize = pTableMetaInfo->pTableMeta->tableInfo.rowSize;
      addExprInfoParam(&pExpr->base, (char*) &rowSize, TSDB_DATA_TYPE_BIGINT, 8);
      return TSDB_CODE_SUCCESS;
    }

    default: {
//      pUdfInfo = isValidUdf(pQueryInfo->pUdfInfo, pItem->pNode->Expr.operand.z, pItem->pNode->Expr.operand.n);
//      if (pUdfInfo == NULL) {
//        return buildInvalidOperationMsg(pMsgBuf, msg9);
//      }

      tSqlExprItem* pParamElem = taosArrayGet(pItem->pNode->Expr.paramList, 0);;
      if (pParamElem->pNode->tokenId != TK_ID) {
        return buildInvalidOperationMsg(pMsgBuf, msg2);
      }

      SColumnIndex index = COLUMN_INDEX_INITIALIZER;
      if (getColumnIndexByName(&pParamElem->pNode->columnName, pQueryInfo, &index, pMsgBuf) != TSDB_CODE_SUCCESS) {
        return buildInvalidOperationMsg(pMsgBuf, msg3);
      }

      if (index.columnIndex == TSDB_TBNAME_COLUMN_INDEX) {
        return buildInvalidOperationMsg(pMsgBuf, msg6);
      }

      pTableMetaInfo = getMetaInfo(pQueryInfo, index.tableIndex);

      // functions can not be applied to tags
      if (index.columnIndex >= getNumOfColumns(pTableMetaInfo->pTableMeta)) {
        return buildInvalidOperationMsg(pMsgBuf, msg6);
      }

      SResultDataInfo resInfo = {0};
      getResultDataInfo(TSDB_DATA_TYPE_INT, 4, functionId, 0, &resInfo, 0, false/*, pUdfInfo*/);

      SSchema s = createSchema(resInfo.type, resInfo.bytes, getNewResColId(), "");
      SSchema* colSchema = getOneColumnSchema(pTableMetaInfo->pTableMeta, index.tableIndex);

      char token[TSDB_COL_NAME_LEN] = {0};
      setTokenAndResColumnName(pItem, s.name, token, sizeof(s.name) - 1);
      doAddOneExprInfo(pQueryInfo, colIndex, functionId, &index, colSchema, &s, NULL, resInfo.intermediateBytes, token, finalResult);
      return TSDB_CODE_SUCCESS;
    }
  }

  return TSDB_CODE_TSC_INVALID_OPERATION;
}

SExprInfo* doAddProjectCol(SQueryStmtInfo* pQueryInfo, int32_t outputColIndex, SColumnIndex* pColIndex, const char* aliasName, int32_t colId) {
  STableMeta* pTableMeta = getMetaInfo(pQueryInfo, pColIndex->tableIndex)->pTableMeta;

  SSchema* pSchema = getOneColumnSchema(pTableMeta, pColIndex->columnIndex);
  SColumnIndex index = *pColIndex;

  int16_t functionId = 0;
  if (TSDB_COL_IS_TAG(index.type)) {
    int32_t numOfCols = getNumOfColumns(pTableMeta);
    index.columnIndex = pColIndex->columnIndex - numOfCols;
    functionId = FUNCTION_TAGPRJ;
  } else {
    index.columnIndex = pColIndex->columnIndex;
    functionId = FUNCTION_PRJ;
  }

  const char* name = (aliasName == NULL)? pSchema->name:aliasName;
  SSchema s = createSchema(pSchema->type, pSchema->bytes, colId, name);
  return doAddOneExprInfo(pQueryInfo, outputColIndex, functionId, &index, pSchema, &s, NULL, 0, pSchema->name, true);
}

static int32_t doAddProjectionExprAndResColumn(SQueryStmtInfo* pQueryInfo, SColumnIndex* pIndex, int32_t startPos) {
  STableMetaInfo* pTableMetaInfo = getMetaInfo(pQueryInfo, pIndex->tableIndex);

  STableMeta* pTableMeta = pTableMetaInfo->pTableMeta;
  STableComInfo tinfo = getTableInfo(pTableMeta);

  int32_t numOfTotalColumns = tinfo.numOfColumns;
  if (UTIL_TABLE_IS_SUPER_TABLE(pTableMetaInfo)) {
    numOfTotalColumns += tinfo.numOfTags;
  }

  for (int32_t j = 0; j < numOfTotalColumns; ++j) {
    pIndex->columnIndex = j;
    doAddProjectCol(pQueryInfo, startPos + j, pIndex, NULL, getNewResColId());
  }

  return numOfTotalColumns;
}

// User input constant value as a new result column
static SColumnIndex createConstantColumnIndex(int32_t* colId) {
  SColumnIndex index = COLUMN_INDEX_INITIALIZER;
  index.columnIndex = ((*colId)--);
  index.tableIndex = 0;
  index.type = TSDB_COL_UDC;
  return index;
}

static SSchema createConstantColumnSchema(SVariant* pVal, const SToken* exprStr, const char* name) {
  SSchema s = {0};

  s.type  = pVal->nType;
  if (IS_VAR_DATA_TYPE(s.type)) {
    s.bytes = (int16_t)(pVal->nLen + VARSTR_HEADER_SIZE);
  } else {
    s.bytes = tDataTypes[pVal->nType].bytes;
  }

  s.colId = TSDB_UD_COLUMN_INDEX;

  if (name != NULL) {
    tstrncpy(s.name, name, sizeof(s.name));
  } else {
    size_t tlen = MIN(sizeof(s.name), exprStr->n + 1);
    tstrncpy(s.name, exprStr->z, tlen);
    strdequote(s.name);
  }

  return s;
}

int32_t addProjectionExprAndResColumn(SQueryStmtInfo* pQueryInfo, tSqlExprItem* pItem, bool outerQuery, SMsgBuf* pMsgBuf) {
  const char* msg1 = "tag for normal table query is not allowed";
  const char* msg2 = "invalid column name";
  const char* msg3 = "tbname not allowed in outer query";

  if (checkForAliasName(pMsgBuf, pItem->aliasName) != TSDB_CODE_SUCCESS) {
    return TSDB_CODE_TSC_INVALID_OPERATION;
  }

  int32_t startPos = (int32_t)getNumOfExprs(pQueryInfo);
  int32_t tokenId = pItem->pNode->tokenId;
  if (tokenId == TK_ALL) {  // project on all fields
    TSDB_QUERY_SET_TYPE(pQueryInfo->type, TSDB_QUERY_TYPE_PROJECTION_QUERY);

    SColumnIndex index = COLUMN_INDEX_INITIALIZER;
    if (getTableIndexByName(&pItem->pNode->columnName, pQueryInfo, &index) != TSDB_CODE_SUCCESS) {
      return buildInvalidOperationMsg(pMsgBuf, msg2);
    }

    // all columns are required
    if (index.tableIndex == COLUMN_INDEX_INITIAL_VAL) {  // all table columns are required.
      for (int32_t i = 0; i < pQueryInfo->numOfTables; ++i) {
        index.tableIndex = i;
        int32_t inc = doAddProjectionExprAndResColumn(pQueryInfo, &index, startPos);
        startPos += inc;
      }
    } else {
      doAddProjectionExprAndResColumn(pQueryInfo, &index, startPos);
    }

    // add the primary timestamp column even though it is not required by user
    STableMeta* pTableMeta = pQueryInfo->pTableMetaInfo[index.tableIndex]->pTableMeta;
    if (pTableMeta->tableType != TSDB_TEMP_TABLE) {
      insertPrimaryTsColumn(pQueryInfo->colList, pTableMeta->uid);
    }
  } else if (tokenId == TK_STRING || tokenId == TK_INTEGER || tokenId == TK_FLOAT) {  // simple column projection query
    SColumnIndex index = createConstantColumnIndex(&pQueryInfo->udColumnId);
    SSchema colSchema = createConstantColumnSchema(&pItem->pNode->value, &pItem->pNode->exprToken, pItem->aliasName);

    char token[TSDB_COL_NAME_LEN] = {0};
    tstrncpy(token, pItem->pNode->exprToken.z, MIN(TSDB_COL_NAME_LEN, TSDB_COL_NAME_LEN));
    SExprInfo* pExpr = doAddOneExprInfo(pQueryInfo, startPos, FUNCTION_PRJ, &index, &colSchema, &colSchema, NULL, 0, token, true);

    // NOTE: the first parameter is reserved for the tag column id during join query process.
    pExpr->base.numOfParams = 2;
    taosVariantAssign(&pExpr->base.param[1], &pItem->pNode->value);
  } else if (tokenId == TK_ID) {
    SColumnIndex index = COLUMN_INDEX_INITIALIZER;
    if (getColumnIndexByName(&pItem->pNode->columnName, pQueryInfo, &index, pMsgBuf) != TSDB_CODE_SUCCESS) {
      return buildInvalidOperationMsg(pMsgBuf, msg2);
    }

    if (index.columnIndex == TSDB_TBNAME_COLUMN_INDEX) {
      SSchema colSchema = {0};
      int32_t functionId = 0;

      if (outerQuery) {  // todo??
        STableMetaInfo* pTableMetaInfo = getMetaInfo(pQueryInfo, index.tableIndex);

        bool     existed = false;
        SSchema* pSchema = pTableMetaInfo->pTableMeta->schema;

        int32_t numOfCols = getNumOfColumns(pTableMetaInfo->pTableMeta);
        for (int32_t i = 0; i < numOfCols; ++i) {
          if (strncasecmp(pSchema[i].name, TSQL_TBNAME_L, tListLen(pSchema[i].name)) == 0) {
            existed = true;
            index.columnIndex = i;
            break;
          }
        }

        if (!existed) {
          return buildInvalidOperationMsg(pMsgBuf, msg3);
        }

        colSchema = pSchema[index.columnIndex];
        functionId = FUNCTION_PRJ;
      } else {
        colSchema = *getTbnameColumnSchema();
        functionId = FUNCTION_TAGPRJ;
      }

      SSchema resultSchema = colSchema;
      resultSchema.colId = getNewResColId();

      char rawName[TSDB_COL_NAME_LEN] = {0};
      setTokenAndResColumnName(pItem, resultSchema.name, rawName, sizeof(colSchema.name) - 1);

      doAddOneExprInfo(pQueryInfo, startPos, functionId, &index, &colSchema, &resultSchema, NULL, 0, rawName, true);
    } else {
      STableMetaInfo* pTableMetaInfo = getMetaInfo(pQueryInfo, index.tableIndex);
      if (TSDB_COL_IS_TAG(index.type) && UTIL_TABLE_IS_NORMAL_TABLE(pTableMetaInfo)) {
        return buildInvalidOperationMsg(pMsgBuf, msg1);
      }

      doAddProjectCol(pQueryInfo, startPos, &index, pItem->aliasName, getNewResColId());
    }

    // add the primary timestamp column even though it is not required by user
    STableMetaInfo* pTableMetaInfo = getMetaInfo(pQueryInfo, index.tableIndex);
    if (!UTIL_TABLE_IS_TMP_TABLE(pTableMetaInfo)) {
      insertPrimaryTsColumn(pQueryInfo->colList, pTableMetaInfo->pTableMeta->uid);
    }
  } else {
    return TSDB_CODE_TSC_INVALID_OPERATION;
  }

  return TSDB_CODE_SUCCESS;
}

static int32_t validateExprLeafNode(tSqlExpr* pExpr, SQueryStmtInfo* pQueryInfo, SArray* pList, int32_t* type, uint64_t* uid,
    SMsgBuf* pMsgBuf) {
  if (pExpr->type == SQL_NODE_TABLE_COLUMN) {
    if (*type == NON_ARITHMEIC_EXPR) {
      *type = NORMAL_ARITHMETIC;
    } else if (*type == AGG_ARIGHTMEIC) {
      return TSDB_CODE_TSC_INVALID_OPERATION;
    }

    SColumnIndex index = COLUMN_INDEX_INITIALIZER;
    if (getColumnIndexByName(&pExpr->columnName, pQueryInfo, &index, pMsgBuf) != TSDB_CODE_SUCCESS) {
      return TSDB_CODE_TSC_INVALID_OPERATION;
    }

    // if column is timestamp, bool, binary, nchar, not support arithmetic, so return invalid sql
    STableMeta* pTableMeta = getMetaInfo(pQueryInfo, index.tableIndex)->pTableMeta;

    SSchema* pSchema = getOneColumnSchema(pTableMeta, index.columnIndex);
    if ((pSchema->type == TSDB_DATA_TYPE_TIMESTAMP) || (pSchema->type == TSDB_DATA_TYPE_BOOL) ||
        (pSchema->type == TSDB_DATA_TYPE_BINARY) || (pSchema->type == TSDB_DATA_TYPE_NCHAR)) {
      return TSDB_CODE_TSC_INVALID_OPERATION;
    }

    taosArrayPush(pList, &index);
  } else if ((pExpr->tokenId == TK_FLOAT && (isnan(pExpr->value.d) || isinf(pExpr->value.d))) ||
             pExpr->tokenId == TK_NULL) {
    return TSDB_CODE_TSC_INVALID_OPERATION;
  } else if (pExpr->type == SQL_NODE_SQLFUNCTION) {
    if (*type == NON_ARITHMEIC_EXPR) {
      *type = AGG_ARIGHTMEIC;
    } else if (*type == NORMAL_ARITHMETIC) {
      return TSDB_CODE_TSC_INVALID_OPERATION;
    }

    tSqlExprItem item = {.pNode = pExpr, .aliasName = NULL};

    // sql function list in selection clause.
    // Append the sqlExpr into exprList of pQueryInfo structure sequentially
    item.functionId = qIsBuiltinFunction(pExpr->Expr.operand.z, pExpr->Expr.operand.n);
    if (item.functionId < 0) {
      return TSDB_CODE_TSC_INVALID_OPERATION;
    }

    int32_t outputIndex = (int32_t)getNumOfExprs(pQueryInfo);
    if (addExprAndResColumn(pQueryInfo, outputIndex, &item, false, pMsgBuf) != TSDB_CODE_SUCCESS) {
      return TSDB_CODE_TSC_INVALID_OPERATION;
    }

    // It is invalid in case of more than one sqlExpr, such as first(ts, k) - last(ts, k)
    int32_t inc = (int32_t) getNumOfExprs(pQueryInfo) - outputIndex;
    if (inc > 1) {
      return TSDB_CODE_TSC_INVALID_OPERATION;
    }

    // Not supported data type in arithmetic expression
    uint64_t id = -1;
    for(int32_t i = 0; i < inc; ++i) {
      SExprInfo* p1 = getExprInfo(pQueryInfo, i + outputIndex);

      int16_t t = p1->base.resSchema.type;
      if (IS_VAR_DATA_TYPE(t) || t == TSDB_DATA_TYPE_BOOL || t == TSDB_DATA_TYPE_TIMESTAMP) {
        return TSDB_CODE_TSC_INVALID_OPERATION;
      }

      if (i == 0) {
        id = p1->base.uid;
        continue;
      }

      if (id != p1->base.uid) {
        return TSDB_CODE_TSC_INVALID_OPERATION;
      }
    }

    *uid = id;
  }

  return TSDB_CODE_SUCCESS;
}

int32_t validateComplexExpr(tSqlExpr* pExpr, SQueryStmtInfo* pQueryInfo, SArray* pColList, int32_t* type, SMsgBuf* pMsgBuf) {
  if (pExpr == NULL) {
    return TSDB_CODE_SUCCESS;
  }

  tSqlExpr* pLeft = pExpr->pLeft;
  uint64_t uidLeft = 0;
  uint64_t uidRight = 0;

  if (pLeft->type == SQL_NODE_EXPR) {
    int32_t ret = validateComplexExpr(pLeft, pQueryInfo, pColList, type, pMsgBuf);
    if (ret != TSDB_CODE_SUCCESS) {
      return ret;
    }
  } else {
    int32_t ret = validateExprLeafNode(pLeft, pQueryInfo, pColList, type, &uidLeft, pMsgBuf);
    if (ret != TSDB_CODE_SUCCESS) {
      return ret;
    }
  }

  tSqlExpr* pRight = pExpr->pRight;
  if (pRight->type == SQL_NODE_EXPR) {
    int32_t ret = validateComplexExpr(pRight, pQueryInfo, pColList, type, pMsgBuf);
    if (ret != TSDB_CODE_SUCCESS) {
      return ret;
    }
  } else {
    int32_t ret = validateExprLeafNode(pRight, pQueryInfo, pColList, type, &uidRight, pMsgBuf);
    if (ret != TSDB_CODE_SUCCESS) {
      return ret;
    }
  }

  return TSDB_CODE_SUCCESS;
}

int32_t  sqlExprToExprNode(tExprNode **pExpr, const tSqlExpr* pSqlExpr, SQueryStmtInfo* pQueryInfo, SArray* pCols, uint64_t *uid, SMsgBuf* pMsgBuf) {
  tExprNode* pLeft = NULL;
  tExprNode* pRight= NULL;

  SColumnIndex index = COLUMN_INDEX_INITIALIZER;
  if (pSqlExpr->pLeft != NULL) {
    int32_t ret = sqlExprToExprNode(&pLeft, pSqlExpr->pLeft, pQueryInfo, pCols, uid, pMsgBuf);
    if (ret != TSDB_CODE_SUCCESS) {
      return ret;
    }
  }

  if (pSqlExpr->pRight != NULL) {
    int32_t ret = sqlExprToExprNode(&pRight, pSqlExpr->pRight, pQueryInfo, pCols, uid, pMsgBuf);
    if (ret != TSDB_CODE_SUCCESS) {
      tExprTreeDestroy(pLeft, NULL);
      return ret;
    }
  }

  if (pSqlExpr->pLeft == NULL && pSqlExpr->pRight == NULL && pSqlExpr->tokenId == 0) {
    *pExpr = calloc(1, sizeof(tExprNode));
    return TSDB_CODE_SUCCESS;
  }

  if (pSqlExpr->pLeft == NULL) {  // it is the leaf node
    assert(pSqlExpr->pRight == NULL);

    if (pSqlExpr->type == SQL_NODE_VALUE) {
      int32_t ret = TSDB_CODE_SUCCESS;
      *pExpr = calloc(1, sizeof(tExprNode));
      (*pExpr)->nodeType = TEXPR_VALUE_NODE;
      (*pExpr)->pVal = calloc(1, sizeof(SVariant));
      taosVariantAssign((*pExpr)->pVal, &pSqlExpr->value);

      STableMeta* pTableMeta = getMetaInfo(pQueryInfo, 0)->pTableMeta;
      if (pCols != NULL && taosArrayGetSize(pCols) > 0) {
        SColIndex* idx = taosArrayGet(pCols, 0);
        SSchema* pSchema = getOneColumnSchema(pTableMeta, idx->colIndex);

        // convert time by precision
        if (pSchema != NULL && TSDB_DATA_TYPE_TIMESTAMP == pSchema->type && TSDB_DATA_TYPE_BINARY == (*pExpr)->pVal->nType) {
#if 0
          ret = setColumnFilterInfoForTimestamp(pCmd, pQueryInfo, (*pExpr)->pVal);
#endif
        }
      }
      return ret;
    } else if (pSqlExpr->type == SQL_NODE_SQLFUNCTION) {
      // arithmetic expression on the results of aggregation functions
      *pExpr = calloc(1, sizeof(tExprNode));
      (*pExpr)->nodeType = TEXPR_COL_NODE;
      (*pExpr)->pSchema = calloc(1, sizeof(SSchema));
      strncpy((*pExpr)->pSchema->name, pSqlExpr->exprToken.z, pSqlExpr->exprToken.n);

      // set the input column data byte and type.
      size_t size = taosArrayGetSize(pQueryInfo->exprList);

      for (int32_t i = 0; i < size; ++i) {
        SExprInfo* p1 = taosArrayGetP(pQueryInfo->exprList, i);

        if (strcmp((*pExpr)->pSchema->name, p1->base.resSchema.name) == 0) {
          memcpy((*pExpr)->pSchema, &p1->base.resSchema, sizeof(SSchema));
          if (uid != NULL) {
            *uid = p1->base.uid;
          }

          break;
        }
      }
    } else if (pSqlExpr->type == SQL_NODE_TABLE_COLUMN) { // column name, normal column arithmetic expression
      int32_t ret = getColumnIndexByName(&pSqlExpr->columnName, pQueryInfo, &index, pMsgBuf);
      if (ret != TSDB_CODE_SUCCESS) {
        return ret;
      }

      pQueryInfo->curTableIdx = index.tableIndex;
      STableMeta* pTableMeta = getMetaInfo(pQueryInfo, index.tableIndex)->pTableMeta;

      *pExpr = calloc(1, sizeof(tExprNode));
      (*pExpr)->nodeType = TEXPR_COL_NODE;
      (*pExpr)->pSchema = calloc(1, sizeof(SSchema));

      SSchema* pSchema = getOneColumnSchema(pTableMeta, index.columnIndex);
      *(*pExpr)->pSchema = *pSchema;

      if (pCols != NULL) {  // record the involved columns
        SColIndex colIndex = {0};
        tstrncpy(colIndex.name, pSchema->name, sizeof(colIndex.name));
        colIndex.colId = pSchema->colId;
        colIndex.colIndex = index.columnIndex;
        colIndex.flag = index.type;

        taosArrayPush(pCols, &colIndex);
      }

      return TSDB_CODE_SUCCESS;
    } else if (pSqlExpr->tokenId == TK_SET) {
      int32_t colType = -1;
      STableMeta* pTableMeta = getMetaInfo(pQueryInfo, pQueryInfo->curTableIdx)->pTableMeta;
      if (pCols != NULL) {
        size_t colSize = taosArrayGetSize(pCols);

        if (colSize > 0) {
          SColIndex* idx = taosArrayGet(pCols, colSize - 1);
          SSchema* pSchema = getOneColumnSchema(pTableMeta, idx->colIndex);
          if (pSchema != NULL) {
            colType = pSchema->type;
          }
        }
      }

      SVariant *pVal;
      if (colType >= TSDB_DATA_TYPE_TINYINT && colType <= TSDB_DATA_TYPE_BIGINT) {
        colType = TSDB_DATA_TYPE_BIGINT;
      } else if (colType == TSDB_DATA_TYPE_FLOAT || colType == TSDB_DATA_TYPE_DOUBLE) {
        colType = TSDB_DATA_TYPE_DOUBLE;
      }
      STableMetaInfo* pTableMetaInfo = getMetaInfo(pQueryInfo, pQueryInfo->curTableIdx);
      STableComInfo tinfo = getTableInfo(pTableMetaInfo->pTableMeta);
#if 0
      if (serializeExprListToVariant(pSqlExpr->Expr.paramList, &pVal, colType, tinfo.precision) == false) {
        return buildInvalidOperationMsg(pMsgBuf, "not support filter expression");
      }
#endif
      *pExpr = calloc(1, sizeof(tExprNode));
      (*pExpr)->nodeType = TEXPR_VALUE_NODE;
      (*pExpr)->pVal = pVal;
    } else {
      return buildInvalidOperationMsg(pMsgBuf, "not support filter expression");
    }

  } else {
    *pExpr = (tExprNode *)calloc(1, sizeof(tExprNode));
    (*pExpr)->nodeType = TEXPR_BINARYEXPR_NODE;

    (*pExpr)->_node.pLeft = pLeft;
    (*pExpr)->_node.pRight = pRight;

    SToken t = {.type = pSqlExpr->tokenId};
    (*pExpr)->_node.optr = convertRelationalOperator(&t);

    assert((*pExpr)->_node.optr != 0);

    // check for dividing by 0
    if ((*pExpr)->_node.optr == TSDB_BINARY_OP_DIVIDE) {
      if (pRight->nodeType == TEXPR_VALUE_NODE) {
        if (pRight->pVal->nType == TSDB_DATA_TYPE_INT && pRight->pVal->i == 0) {
          return buildInvalidOperationMsg(pMsgBuf, "invalid expr (divide by 0)");
        } else if (pRight->pVal->nType == TSDB_DATA_TYPE_FLOAT && pRight->pVal->d == 0) {
          return buildInvalidOperationMsg(pMsgBuf, "invalid expr (divide by 0)");
        }
      }
    }

    // NOTE: binary|nchar data allows the >|< type filter
    if ((*pExpr)->_node.optr != TSDB_RELATION_EQUAL && (*pExpr)->_node.optr != TSDB_RELATION_NOT_EQUAL) {
      if (pRight != NULL && pRight->nodeType == TEXPR_VALUE_NODE) {
        if (pRight->pVal->nType == TSDB_DATA_TYPE_BOOL && pLeft->pSchema->type == TSDB_DATA_TYPE_BOOL) {
          return buildInvalidOperationMsg(pMsgBuf, "invalid operator for bool");
        }
      }
    }
  }

  return TSDB_CODE_SUCCESS;
}

static int32_t multiColumnListInsert(SQueryStmtInfo* pQueryInfo, SArray* pColumnList, SMsgBuf* pMsgBuf) {
  const char* msg3 = "tag columns can not be used in arithmetic expression";

  SColumnIndex* p1 = taosArrayGet(pColumnList, 0);
  STableMeta* pTableMeta = getMetaInfo(pQueryInfo, p1->tableIndex)->pTableMeta;

  size_t numOfNode = taosArrayGetSize(pColumnList);
  for(int32_t k = 0; k < numOfNode; ++k) {
    SColumnIndex* pIndex = taosArrayGet(pColumnList, k);
    if (TSDB_COL_IS_TAG(pIndex->type)) {
      return buildInvalidOperationMsg(pMsgBuf, msg3);
    }

    SSchema* ps = getOneColumnSchema(pTableMeta, pIndex->columnIndex);
    columnListInsert(pQueryInfo->colList, pIndex->columnIndex, pTableMeta->uid, ps);
  }

  insertPrimaryTsColumn(pQueryInfo->colList, pTableMeta->uid);
  return TSDB_CODE_SUCCESS;
}

static int32_t createComplexExpr(SQueryStmtInfo* pQueryInfo, int32_t exprIndex, tSqlExprItem* pItem, SMsgBuf* pMsgBuf) {
  const char* msg1 = "invalid column name, illegal column type, or columns in arithmetic expression from two tables";
  const char* msg2 = "invalid arithmetic expression in select clause";
  const char* msg3 = "tag columns can not be used in arithmetic expression";

  int32_t arithmeticType = NON_ARITHMEIC_EXPR;
  SArray* pColumnList = taosArrayInit(4, sizeof(SColumnIndex));
  if (validateComplexExpr(pItem->pNode, pQueryInfo, pColumnList, &arithmeticType, pMsgBuf) != TSDB_CODE_SUCCESS) {
    return buildInvalidOperationMsg(pMsgBuf, msg1);
  }

  if (arithmeticType == NORMAL_ARITHMETIC) {
    // expr string is set as the parameter of function
    SSchema s = createSchema(TSDB_DATA_TYPE_DOUBLE, sizeof(double), getNewResColId(), "");

    tExprNode* pNode = NULL;
    SArray* colList = taosArrayInit(10, sizeof(SColIndex));
    int32_t ret = sqlExprToExprNode(&pNode, pItem->pNode, pQueryInfo, colList, NULL, pMsgBuf);
    if (ret != TSDB_CODE_SUCCESS) {
      taosArrayDestroy(colList);
      tExprTreeDestroy(pNode, NULL);
      return buildInvalidOperationMsg(pMsgBuf, msg2);
    }

    SExprInfo* pExpr = createBinaryExprInfo(pNode, &s);
    addExprInfo(pQueryInfo, exprIndex, pExpr);
    setTokenAndResColumnName(pItem, pExpr->base.resSchema.name, pExpr->base.token, TSDB_COL_NAME_LEN);

    // check for if there is a tag in the arithmetic express
    int32_t code = multiColumnListInsert(pQueryInfo, pColumnList, pMsgBuf);
    if (code != TSDB_CODE_SUCCESS) {
      taosArrayDestroy(colList);
      tExprTreeDestroy(pNode, NULL);
      return code;
    }

    SBufferWriter bw = tbufInitWriter(NULL, false);

//    TRY(0) {
      exprTreeToBinary(&bw, pNode);
//    } CATCH(code) {
//      tbufCloseWriter(&bw);
//      UNUSED(code);
//       TODO: other error handling
//    } END_TRY

    int32_t len = tbufTell(&bw);
    char* c = tbufGetData(&bw, false);

    // set the serialized binary string as the parameter of arithmetic expression
    addExprInfoParam(&pExpr->base, c, TSDB_DATA_TYPE_BINARY, (int32_t)len);
    addResColumnInfo(pQueryInfo, exprIndex, &pExpr->base.resSchema, pExpr);

    tbufCloseWriter(&bw);
    taosArrayDestroy(colList);
  } else {
    SColumnIndex  columnIndex = {0};

    SSchema s = createSchema(TSDB_DATA_TYPE_DOUBLE, sizeof(double), getNewResColId(), "");
    addResColumnInfo(pQueryInfo, exprIndex, &s, NULL);

    tExprNode* pNode = NULL;
    int32_t ret = sqlExprToExprNode(&pNode, pItem->pNode, pQueryInfo, NULL, NULL, pMsgBuf);
    if (ret != TSDB_CODE_SUCCESS) {
      tExprTreeDestroy(pNode, NULL);
      return buildInvalidOperationMsg(pMsgBuf, "invalid expression in select clause");
    }

    SExprInfo* pExpr = createBinaryExprInfo(pNode, &s);
    addExprInfo(pQueryInfo, exprIndex, pExpr);
    setTokenAndResColumnName(pItem, pExpr->base.resSchema.name, pExpr->base.token, TSDB_COL_NAME_LEN);

    pExpr->base.numOfParams = 1;

    SBufferWriter bw = tbufInitWriter(NULL, false);
//    TRY(0) {
      exprTreeToBinary(&bw, pExpr->pExpr);
//    } CATCH(code) {
//      tbufCloseWriter(&bw);
//      UNUSED(code);
//       TODO: other error handling
//    } END_TRY

    SSqlExpr* pSqlExpr = &pExpr->base;
    pSqlExpr->param[0].nLen = (int16_t) tbufTell(&bw);
    pSqlExpr->param[0].pz   = tbufGetData(&bw, true);
    pSqlExpr->param[0].nType = TSDB_DATA_TYPE_BINARY;

    tbufCloseWriter(&bw);

//    tbufCloseWriter(&bw); // TODO there is a memory leak
  }

  taosArrayDestroy(pColumnList);
  return TSDB_CODE_SUCCESS;
}

int32_t validateSelectNodeList(SQueryStmtInfo* pQueryInfo, SArray* pSelNodeList, bool outerQuery, SMsgBuf* pMsgBuf) {
  assert(pSelNodeList != NULL);

  const char* msg1 = "too many items in selection clause";
  const char* msg2 = "functions or others can not be mixed up";
  const char* msg3 = "not support query expression";
  const char* msg4 = "distinct should be in the first place in select clause";
  const char* msg5 = "invalid function name";

  // too many result columns not support order by in query
  if (taosArrayGetSize(pSelNodeList) > TSDB_MAX_COLUMNS) {
    return buildInvalidOperationMsg(pMsgBuf, msg1);
  }

  size_t numOfExpr = taosArrayGetSize(pSelNodeList);

  for (int32_t i = 0; i < numOfExpr; ++i) {
    int32_t outputIndex = (int32_t)getNumOfExprs(pQueryInfo);
    tSqlExprItem* pItem = taosArrayGet(pSelNodeList, i);
    int32_t type = pItem->pNode->type;

    if (pItem->distinct) {
      if (i != 0/* || type == SQL_NODE_SQLFUNCTION || type == SQL_NODE_EXPR*/) {
        return buildInvalidOperationMsg(pMsgBuf, msg4);
      }

      pQueryInfo->info.distinct = true;
    }

    if (type == SQL_NODE_SQLFUNCTION) {
      pItem->functionId = qIsBuiltinFunction(pItem->pNode->Expr.operand.z, pItem->pNode->Expr.operand.n);
      if (pItem->functionId == FUNCTION_INVALID_ID) {
        int32_t functionId = FUNCTION_INVALID_ID;
        bool valid = qIsValidUdf(pQueryInfo->pUdfInfo, pItem->pNode->Expr.operand.z, pItem->pNode->Expr.operand.n, &functionId);
        if (!valid) {
          return buildInvalidOperationMsg(pMsgBuf, msg5);
        }

        pItem->functionId = functionId;
      }

      // sql function in selection clause, append sql function info in pSqlCmd structure sequentially
      if (addExprAndResColumn(pQueryInfo, outputIndex, pItem, true, pMsgBuf) != TSDB_CODE_SUCCESS) {
        return TSDB_CODE_TSC_INVALID_OPERATION;
      }
    } else if (type == SQL_NODE_TABLE_COLUMN || type == SQL_NODE_VALUE) {
      // use the dynamic array list to decide if the function is valid or not
      // select table_name1.field_name1, table_name2.field_name2  from table_name1, table_name2
      if (addProjectionExprAndResColumn(pQueryInfo, pItem, outerQuery, pMsgBuf) != TSDB_CODE_SUCCESS) {
        return TSDB_CODE_TSC_INVALID_OPERATION;
      }
    } else if (type == SQL_NODE_EXPR) {
      int32_t code = createComplexExpr(pQueryInfo, i, pItem, pMsgBuf);
      if (code != TSDB_CODE_SUCCESS) {
        return code;
      }
    } else {
      return buildInvalidOperationMsg(pMsgBuf, msg3);
    }
  }

  return TSDB_CODE_SUCCESS;
}

int32_t evaluateSqlNode(SSqlNode* pNode, int32_t tsPrecision, SMsgBuf* pMsgBuf) {
  assert(pNode != NULL && pMsgBuf != NULL && pMsgBuf->len > 0);

  // Evaluate expression in where clause
  if (pNode->pWhere != NULL) {
    int32_t code = evaluateSqlNodeImpl(pNode->pWhere, tsPrecision);
    if (code != TSDB_CODE_SUCCESS) {
      strncpy(pMsgBuf->buf, "invalid time expression in sql", pMsgBuf->len);
      return code;
    }
  }

  // Evaluate the expression in select clause
  size_t size = taosArrayGetSize(pNode->pSelNodeList);
  for(int32_t i = 0; i < size; ++i) {
    tSqlExprItem* pItem = taosArrayGet(pNode->pSelNodeList, i);
    int32_t code = evaluateSqlNodeImpl(pItem->pNode, tsPrecision);
    if (code != TSDB_CODE_SUCCESS) {
      return code;
    }
  }

  return TSDB_CODE_SUCCESS;
}

int32_t qParserValidateSqlNode(struct SCatalog* pCatalog, SSqlInfo* pInfo, SQueryStmtInfo* pQueryInfo, int64_t id, char* msgBuf, int32_t msgBufLen) {
  //1. if it is a query, get the meta info and continue.
  assert(pCatalog != NULL && pInfo != NULL);
  int32_t code = 0;
#if 0
  switch (pInfo->type) {
    case TSDB_SQL_DROP_TABLE:
    case TSDB_SQL_DROP_USER:
    case TSDB_SQL_DROP_ACCT:
    case TSDB_SQL_DROP_DNODE:
    case TSDB_SQL_DROP_DB: {
      const char* msg1 = "param name too long";
      const char* msg2 = "invalid name";

      SToken* pzName = taosArrayGet(pInfo->pMiscInfo->a, 0);
      if ((pInfo->type != TSDB_SQL_DROP_DNODE) && (parserValidateIdToken(pzName) != TSDB_CODE_SUCCESS)) {
        return setInvalidOperatorMsg(pMsgBuf, msg2);
      }

      if (pInfo->type == TSDB_SQL_DROP_DB) {
        assert(taosArrayGetSize(pInfo->pMiscInfo->a) == 1);
        code = tNameSetDbName(&pTableMetaInfo->name, getAccountId(pSql), pzName);
        if (code != TSDB_CODE_SUCCESS) {
          return setInvalidOperatorMsg(pMsgBuf, msg2);
        }

      } else if (pInfo->type == TSDB_SQL_DROP_TABLE) {
        assert(taosArrayGetSize(pInfo->pMiscInfo->a) == 1);

        code = tscSetTableFullName(&pTableMetaInfo->name, pzName, pSql);
        if(code != TSDB_CODE_SUCCESS) {
          return code;
        }
      } else if (pInfo->type == TSDB_SQL_DROP_DNODE) {
        if (pzName->type == TK_STRING) {
          pzName->n = strdequote(pzName->z);
        }
        strncpy(pCmd->payload, pzName->z, pzName->n);
      } else {  // drop user/account
        if (pzName->n >= TSDB_USER_LEN) {
          return setInvalidOperatorMsg(pMsgBuf, msg3);
        }

        strncpy(pCmd->payload, pzName->z, pzName->n);
      }

      break;
    }

    case TSDB_SQL_USE_DB: {
      const char* msg = "invalid db name";
      SToken* pToken = taosArrayGet(pInfo->pMiscInfo->a, 0);

      if (tscValidateName(pToken) != TSDB_CODE_SUCCESS) {
        return setInvalidOperatorMsg(pMsgBuf, msg);
      }

      int32_t ret = tNameSetDbName(&pTableMetaInfo->name, getAccountId(pSql), pToken);
      if (ret != TSDB_CODE_SUCCESS) {
        return setInvalidOperatorMsg(pMsgBuf, msg);
      }

      break;
    }

    case TSDB_SQL_RESET_CACHE: {
      return TSDB_CODE_SUCCESS;
    }

    case TSDB_SQL_SHOW: {
      if (setShowInfo(pSql, pInfo) != TSDB_CODE_SUCCESS) {
        return TSDB_CODE_TSC_INVALID_OPERATION;
      }

      break;
    }

    case TSDB_SQL_CREATE_FUNCTION:
    case TSDB_SQL_DROP_FUNCTION:  {
      code = handleUserDefinedFunc(pSql, pInfo);
      if (code != TSDB_CODE_SUCCESS) {
        return code;
      }

      break;
    }

    case TSDB_SQL_ALTER_DB:
    case TSDB_SQL_CREATE_DB: {
      const char* msg1 = "invalid db name";
      const char* msg2 = "name too long";

      SCreateDbInfo* pCreateDB = &(pInfo->pMiscInfo->dbOpt);
      if (pCreateDB->dbname.n >= TSDB_DB_NAME_LEN) {
        return setInvalidOperatorMsg(pMsgBuf, msg2);
      }

      char buf[TSDB_DB_NAME_LEN] = {0};
      SToken token = taosTokenDup(&pCreateDB->dbname, buf, tListLen(buf));

      if (tscValidateName(&token) != TSDB_CODE_SUCCESS) {
        return setInvalidOperatorMsg(pMsgBuf, msg1);
      }

      int32_t ret = tNameSetDbName(&pTableMetaInfo->name, getAccountId(pSql), &token);
      if (ret != TSDB_CODE_SUCCESS) {
        return setInvalidOperatorMsg(pMsgBuf, msg2);
      }

      if (parseCreateDBOptions(pCmd, pCreateDB) != TSDB_CODE_SUCCESS) {
        return TSDB_CODE_TSC_INVALID_OPERATION;
      }

      break;
    }

    case TSDB_SQL_CREATE_DNODE: {
      const char* msg = "invalid host name (ip address)";

      if (taosArrayGetSize(pInfo->pMiscInfo->a) > 1) {
        return setInvalidOperatorMsg(pMsgBuf, msg);
      }

      SToken* id = taosArrayGet(pInfo->pMiscInfo->a, 0);
      if (id->type == TK_STRING) {
        id->n = strdequote(id->z);
      }
      break;
    }

    case TSDB_SQL_CREATE_ACCT:
    case TSDB_SQL_ALTER_ACCT: {
      const char* msg1 = "invalid state option, available options[no, r, w, all]";
      const char* msg2 = "invalid user/account name";
      const char* msg3 = "name too long";

      SToken* pName = &pInfo->pMiscInfo->user.user;
      SToken* pPwd = &pInfo->pMiscInfo->user.passwd;

      if (handlePassword(pCmd, pPwd) != TSDB_CODE_SUCCESS) {
        return TSDB_CODE_TSC_INVALID_OPERATION;
      }

      if (pName->n >= TSDB_USER_LEN) {
        return setInvalidOperatorMsg(pMsgBuf, msg3);
      }

      if (tscValidateName(pName) != TSDB_CODE_SUCCESS) {
        return setInvalidOperatorMsg(pMsgBuf, msg2);
      }

      SCreateAcctInfo* pAcctOpt = &pInfo->pMiscInfo->acctOpt;
      if (pAcctOpt->stat.n > 0) {
        if (pAcctOpt->stat.z[0] == 'r' && pAcctOpt->stat.n == 1) {
        } else if (pAcctOpt->stat.z[0] == 'w' && pAcctOpt->stat.n == 1) {
        } else if (strncmp(pAcctOpt->stat.z, "all", 3) == 0 && pAcctOpt->stat.n == 3) {
        } else if (strncmp(pAcctOpt->stat.z, "no", 2) == 0 && pAcctOpt->stat.n == 2) {
        } else {
          return setInvalidOperatorMsg(pMsgBuf, msg1);
        }
      }

      break;
    }

    case TSDB_SQL_DESCRIBE_TABLE: {
      const char* msg1 = "invalid table name";

      SToken* pToken = taosArrayGet(pInfo->pMiscInfo->a, 0);
      if (tscValidateName(pToken) != TSDB_CODE_SUCCESS) {
        return setInvalidOperatorMsg(pMsgBuf, msg1);
      }
      // additional msg has been attached already
      code = tscSetTableFullName(&pTableMetaInfo->name, pToken, pSql);
      if (code != TSDB_CODE_SUCCESS) {
        return code;
      }

      return tscGetTableMeta(pSql, pTableMetaInfo);
    }
    case TSDB_SQL_SHOW_CREATE_STABLE:
    case TSDB_SQL_SHOW_CREATE_TABLE: {
      const char* msg1 = "invalid table name";

      SToken* pToken = taosArrayGet(pInfo->pMiscInfo->a, 0);
      if (tscValidateName(pToken) != TSDB_CODE_SUCCESS) {
        return setInvalidOperatorMsg(pMsgBuf, msg1);
      }

      code = tscSetTableFullName(&pTableMetaInfo->name, pToken, pSql);
      if (code != TSDB_CODE_SUCCESS) {
        return code;
      }

      return tscGetTableMeta(pSql, pTableMetaInfo);
    }
    case TSDB_SQL_SHOW_CREATE_DATABASE: {
      const char* msg1 = "invalid database name";

      SToken* pToken = taosArrayGet(pInfo->pMiscInfo->a, 0);
      if (tscValidateName(pToken) != TSDB_CODE_SUCCESS) {
        return setInvalidOperatorMsg(pMsgBuf, msg1);
      }

      if (pToken->n > TSDB_DB_NAME_LEN) {
        return setInvalidOperatorMsg(pMsgBuf, msg1);
      }
      return tNameSetDbName(&pTableMetaInfo->name, getAccountId(pSql), pToken);
    }
    case TSDB_SQL_CFG_DNODE: {
      const char* msg2 = "invalid configure options or values, such as resetlog / debugFlag 135 / balance 'vnode:2-dnode:2' / monitor 1 ";
      const char* msg3 = "invalid dnode ep";

      /* validate the ip address */
      SMiscInfo* pMiscInfo = pInfo->pMiscInfo;

      /* validate the parameter names and options */
      if (validateDNodeConfig(pMiscInfo) != TSDB_CODE_SUCCESS) {
        return setInvalidOperatorMsg(pMsgBuf, msg2);
      }

      char* pMsg = pCmd->payload;

      SCfgDnodeMsg* pCfg = (SCfgDnodeMsg*)pMsg;

      SToken* t0 = taosArrayGet(pMiscInfo->a, 0);
      SToken* t1 = taosArrayGet(pMiscInfo->a, 1);

      t0->n = strdequote(t0->z);
      strncpy(pCfg->ep, t0->z, t0->n);

      if (validateEp(pCfg->ep) != TSDB_CODE_SUCCESS) {
        return setInvalidOperatorMsg(pMsgBuf, msg3);
      }

      strncpy(pCfg->config, t1->z, t1->n);

      if (taosArrayGetSize(pMiscInfo->a) == 3) {
        SToken* t2 = taosArrayGet(pMiscInfo->a, 2);

        pCfg->config[t1->n] = ' ';  // add sep
        strncpy(&pCfg->config[t1->n + 1], t2->z, t2->n);
      }

      break;
    }

    case TSDB_SQL_CREATE_USER:
    case TSDB_SQL_ALTER_USER: {
      const char* msg2 = "invalid user/account name";
      const char* msg3 = "name too long";
      const char* msg5 = "invalid user rights";
      const char* msg7 = "not support options";

      pCmd->command = pInfo->type;

      SUserInfo* pUser = &pInfo->pMiscInfo->user;
      SToken* pName = &pUser->user;
      SToken* pPwd = &pUser->passwd;

      if (pName->n >= TSDB_USER_LEN) {
        return setInvalidOperatorMsg(pMsgBuf, msg3);
      }

      if (tscValidateName(pName) != TSDB_CODE_SUCCESS) {
        return setInvalidOperatorMsg(pMsgBuf, msg2);
      }

      if (pCmd->command == TSDB_SQL_CREATE_USER) {
        if (handlePassword(pCmd, pPwd) != TSDB_CODE_SUCCESS) {
          return TSDB_CODE_TSC_INVALID_OPERATION;
        }
      } else {
        if (pUser->type == TSDB_ALTER_USER_PASSWD) {
          if (handlePassword(pCmd, pPwd) != TSDB_CODE_SUCCESS) {
            return TSDB_CODE_TSC_INVALID_OPERATION;
          }
        } else if (pUser->type == TSDB_ALTER_USER_PRIVILEGES) {
          assert(pPwd->type == TSDB_DATA_TYPE_NULL);

          SToken* pPrivilege = &pUser->privilege;

          if (strncasecmp(pPrivilege->z, "super", 5) == 0 && pPrivilege->n == 5) {
            pCmd->count = 1;
          } else if (strncasecmp(pPrivilege->z, "read", 4) == 0 && pPrivilege->n == 4) {
            pCmd->count = 2;
          } else if (strncasecmp(pPrivilege->z, "write", 5) == 0 && pPrivilege->n == 5) {
            pCmd->count = 3;
          } else {
            return setInvalidOperatorMsg(pMsgBuf, msg5);
          }
        } else {
          return setInvalidOperatorMsg(pMsgBuf, msg7);
        }
      }

      break;
    }

    case TSDB_SQL_CFG_LOCAL: {
      SMiscInfo  *pMiscInfo = pInfo->pMiscInfo;
      const char *msg = "invalid configure options or values";

      // validate the parameter names and options
      if (validateLocalConfig(pMiscInfo) != TSDB_CODE_SUCCESS) {
        return setInvalidOperatorMsg(pMsgBuf, msg);
      }

      int32_t numOfToken = (int32_t) taosArrayGetSize(pMiscInfo->a);
      assert(numOfToken >= 1 && numOfToken <= 2);

      SToken* t = taosArrayGet(pMiscInfo->a, 0);
      strncpy(pCmd->payload, t->z, t->n);
      if (numOfToken == 2) {
        SToken* t1 = taosArrayGet(pMiscInfo->a, 1);
        pCmd->payload[t->n] = ' ';  // add sep
        strncpy(&pCmd->payload[t->n + 1], t1->z, t1->n);
      }
      return TSDB_CODE_SUCCESS;
    }

    case TSDB_SQL_CREATE_TABLE: {
      SCreateTableSql* pCreateTable = pInfo->pCreateTableInfo;

      if (pCreateTable->type == TSQL_CREATE_TABLE || pCreateTable->type == TSQL_CREATE_STABLE) {
        if ((code = doCheckForCreateTable(pSql, 0, pInfo)) != TSDB_CODE_SUCCESS) {
          return code;
        }

      } else if (pCreateTable->type == TSQL_CREATE_TABLE_FROM_STABLE) {
        assert(pCmd->numOfCols == 0);
        if ((code = doCheckForCreateFromStable(pSql, pInfo)) != TSDB_CODE_SUCCESS) {
          return code;
        }

      } else if (pCreateTable->type == TSQL_CREATE_STREAM) {
        if ((code = doCheckForStream(pSql, pInfo)) != TSDB_CODE_SUCCESS) {
          return code;
        }
      }

      break;
    }

    case TSDB_SQL_SELECT: {
      const char * msg1 = "no nested query supported in union clause";
      code = loadAllTableMeta(pSql, pInfo);
      if (code != TSDB_CODE_SUCCESS) {
        return code;
      }

      pQueryInfo = tscGetQueryInfo(pCmd);

      size_t size = taosArrayGetSize(pInfo->list);
      for (int32_t i = 0; i < size; ++i) {
        SSqlNode* pSqlNode = taosArrayGetP(pInfo->list, i);

        tscTrace("0x%"PRIx64" start to parse the %dth subclause, total:%"PRIzu, pSql->self, i, size);

        if (size > 1 && pSqlNode->from && pSqlNode->from->type == SQL_NODE_FROM_SUBQUERY) {
          return setInvalidOperatorMsg(pMsgBuf, msg1);
        }

//        normalizeSqlNode(pSqlNode); // normalize the column name in each function
        if ((code = validateSqlNode(pSql, pSqlNode, pQueryInfo)) != TSDB_CODE_SUCCESS) {
          return code;
        }

        tscPrintSelNodeList(pSql, i);

        if ((i + 1) < size && pQueryInfo->sibling == NULL) {
          if ((code = tscAddQueryInfo(pCmd)) != TSDB_CODE_SUCCESS) {
            return code;
          }

          SArray *pUdfInfo = NULL;
          if (pQueryInfo->pUdfInfo) {
            pUdfInfo = taosArrayDup(pQueryInfo->pUdfInfo);
          }

          pQueryInfo = pCmd->active;
          pQueryInfo->pUdfInfo = pUdfInfo;
          pQueryInfo->udfCopy = true;
        }
      }

      if ((code = normalizeVarDataTypeLength(pCmd)) != TSDB_CODE_SUCCESS) {
        return code;
      }

      // set the command/global limit parameters from the first subclause to the sqlcmd object
      pCmd->active = pCmd->pQueryInfo;
      pCmd->command = pCmd->pQueryInfo->command;

      STableMetaInfo* pTableMetaInfo1 = getMetaInfo(pCmd->active, 0);
      if (pTableMetaInfo1->pTableMeta != NULL) {
        pSql->res.precision = tscGetTableInfo(pTableMetaInfo1->pTableMeta).precision;
      }

      return TSDB_CODE_SUCCESS;  // do not build query message here
    }

    case TSDB_SQL_ALTER_TABLE: {
      if ((code = setAlterTableInfo(pSql, pInfo)) != TSDB_CODE_SUCCESS) {
        return code;
      }

      break;
    }

    case TSDB_SQL_KILL_QUERY:
    case TSDB_SQL_KILL_STREAM:
    case TSDB_SQL_KILL_CONNECTION: {
      if ((code = setKillInfo(pSql, pInfo, pInfo->type)) != TSDB_CODE_SUCCESS) {
        return code;
      }
      break;
    }

    case TSDB_SQL_SYNC_DB_REPLICA: {
      const char* msg1 = "invalid db name";
      SToken* pzName = taosArrayGet(pInfo->pMiscInfo->a, 0);

      assert(taosArrayGetSize(pInfo->pMiscInfo->a) == 1);
      code = tNameSetDbName(&pTableMetaInfo->name, getAccountId(pSql), pzName);
      if (code != TSDB_CODE_SUCCESS) {
        return setInvalidOperatorMsg(pMsgBuf, msg1);
      }
      break;
    }
    case TSDB_SQL_COMPACT_VNODE:{
      const char* msg = "invalid compact";
      if (setCompactVnodeInfo(pSql, pInfo) != TSDB_CODE_SUCCESS) {
        return setInvalidOperatorMsg(pMsgBuf, msg);
      }
      break;
    }
    default:
      return setInvalidOperatorMsg(pMsgBuf, "not support sql expression");
  }
#endif

  SMetaReq req = {0};
  SMetaData data = {0};

  // TODO: check if the qnode info has been cached already
  req.qNodeEpset = true;
  code = qParserExtractRequestedMetaInfo(pInfo, &req, msgBuf, msgBufLen);
  if (code != TSDB_CODE_SUCCESS) {
    return code;
  }

  // load the meta data from catalog
  code = catalogGetMetaData(pCatalog, &req, &data);
  if (code != TSDB_CODE_SUCCESS) {
    return code;
  }

  // evaluate the sqlnode
  STableMeta* pTableMeta = (STableMeta*) taosArrayGetP(data.pTableMeta, 0);
  assert(pTableMeta != NULL);

  SMsgBuf buf = {.buf = msgBuf, .len = msgBufLen};

  size_t len = taosArrayGetSize(pInfo->list);
  for(int32_t i = 0; i < len; ++i) {
    SSqlNode* p = taosArrayGetP(pInfo->list, i);
    code = evaluateSqlNode(p, pTableMeta->tableInfo.precision, &buf);
    if (code != TSDB_CODE_SUCCESS) {
      return code;
    }
  }

  for(int32_t i = 0; i < len; ++i) {
    SSqlNode* p = taosArrayGetP(pInfo->list, i);
    validateSqlNode(p, pQueryInfo, &buf);
  }

  SArray* functionList = extractFunctionIdList(pQueryInfo->exprList);
  extractFunctionDesc(functionList, &pQueryInfo->info);

  if ((code = checkForInvalidExpr(pQueryInfo, &buf)) != TSDB_CODE_SUCCESS) {
    return code;
  }

  // convert the sqlnode into queryinfo
  return code;
}