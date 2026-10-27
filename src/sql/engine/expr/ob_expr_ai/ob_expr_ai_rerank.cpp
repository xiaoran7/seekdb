/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SQL_ENG
#include "ob_expr_ai_rerank.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"
#include "lib/utility/utility.h"
#include "common/json_type/ob_json_common.h"
 
using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{

ObExprAIRerank::ObExprAIRerank(common::ObIAllocator &alloc)
: ObFuncExprOperator(alloc, 
                    T_FUN_SYS_AI_RERANK, 
                    N_AI_RERANK, 
                    MORE_THAN_ZERO,
                    NOT_VALID_FOR_GENERATED_COL, 
                    NOT_ROW_DIMENSION) 
{
}

ObExprAIRerank::~ObExprAIRerank() 
{
}

namespace {
int extract_rerank_index_and_score(ObJsonNode *node,
                                   const int64_t max_count,
                                   int64_t &index_out,
                                   double &score_out,
                                   ObJsonNode *&score_node_out)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(node) || node->json_type() != ObJsonNodeType::J_OBJECT) {
    ret = OB_INVALID_DATA;
  } else {
    ObJsonObject *obj = static_cast<ObJsonObject *>(node);
    ObJsonNode *idx_node = obj->get_value("index");
    ObJsonNode *score_node = obj->get_value("relevance_score");
    if (OB_ISNULL(score_node)) {
      score_node = obj->get_value("score");
    }
    if (OB_ISNULL(idx_node) || OB_ISNULL(score_node)) {
      ret = OB_INVALID_DATA;
    } else if (idx_node->json_type() != ObJsonNodeType::J_INT
               && idx_node->json_type() != ObJsonNodeType::J_UINT) {
      ret = OB_INVALID_DATA;
    } else {
      const int64_t idx = (idx_node->json_type() == ObJsonNodeType::J_INT)
                          ? idx_node->get_int()
                          : static_cast<int64_t>(idx_node->get_uint());
      if ((idx_node->json_type() == ObJsonNodeType::J_UINT && idx_node->get_uint() > static_cast<uint64_t>(INT64_MAX))
          || idx < 0
          || (max_count > 0 && idx >= max_count)) {
        ret = OB_INVALID_DATA;
      } else {
        index_out = idx;
      }
    }
    if (OB_SUCC(ret)) {
      if (score_node->json_type() == ObJsonNodeType::J_DOUBLE) {
        score_out = score_node->get_double();
      } else if (score_node->json_type() == ObJsonNodeType::J_INT) {
        score_out = static_cast<double>(score_node->get_int());
      } else if (score_node->json_type() == ObJsonNodeType::J_UINT) {
        score_out = static_cast<double>(score_node->get_uint());
      } else if (score_node->json_type() == ObJsonNodeType::J_DECIMAL) {
        score_out = score_node->get_double();
      } else {
        ret = OB_INVALID_DATA;
      }
      if (OB_SUCC(ret)) {
        score_node_out = score_node;
      }
    }
  }
  return ret;
}

int sort_rerank_result_array(ObIAllocator &allocator,
                             ObJsonArray *raw_array,
                             const int64_t expected_count,
                             ObJsonArray *&sorted_array)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(raw_array)) {
    ret = OB_INVALID_DATA;
  } else {
    const int64_t cnt = raw_array->element_count();
    if (cnt <= 0 || (expected_count > 0 && cnt > expected_count)) {
      ret = OB_INVALID_DATA;
    } else {
      struct RerankItem {
        int64_t index;
        double score;
        ObJsonObject *obj;
      };
      RerankItem *items = static_cast<RerankItem *>(allocator.alloc(sizeof(RerankItem) * cnt));
      bool *seen = (expected_count > 0)
                   ? static_cast<bool *>(allocator.alloc(sizeof(bool) * expected_count))
                   : nullptr;
      if (OB_ISNULL(items) || (expected_count > 0 && OB_ISNULL(seen))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        if (OB_NOT_NULL(seen)) {
          MEMSET(seen, 0, sizeof(bool) * expected_count);
        }
        for (int64_t i = 0; OB_SUCC(ret) && i < cnt; ++i) {
          ObJsonNode *elem = raw_array->get_value(i);
          int64_t idx = -1;
          double score = 0.0;
          ObJsonNode *score_node = nullptr;
          if (OB_FAIL(extract_rerank_index_and_score(elem, expected_count, idx, score, score_node))) {
          } else if (OB_NOT_NULL(seen) && seen[idx]) {
            ret = OB_INVALID_DATA;
          } else {
            if (OB_NOT_NULL(seen)) {
              seen[idx] = true;
            }
            items[i].index = idx;
            items[i].score = score;
            items[i].obj = static_cast<ObJsonObject *>(elem);
          }
        }
        for (int64_t i = 0; OB_SUCC(ret) && i < cnt; ++i) {
          for (int64_t j = i + 1; j < cnt; ++j) {
            if (items[j].score > items[i].score
                || (items[j].score == items[i].score && items[j].index < items[i].index)) {
              RerankItem tmp = items[i];
              items[i] = items[j];
              items[j] = tmp;
            }
          }
        }
        ObJsonArray *out_arr = nullptr;
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, out_arr))) {
        } else {
          for (int64_t i = 0; OB_SUCC(ret) && i < cnt; ++i) {
            if (OB_FAIL(out_arr->append(items[i].obj))) {
            }
          }
          if (OB_SUCC(ret)) {
            sorted_array = out_arr;
          }
        }
      }
    }
  }
  return ret;
}
} // namespace

int ObExprAIRerank::calc_result_typeN(ObExprResType &type,
                                    ObExprResType *types_stack,
                                    int64_t param_num,
                                    common::ObExprTypeCtx &type_ctx) const {
  UNUSED(type_ctx);
  UNUSED(types_stack);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num < 3 || param_num > 4)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    if (!ob_is_null(types_stack[MODEL_IDX].get_type())
        && !ob_is_string_type(types_stack[MODEL_IDX].get_type())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, model key must be string type");
    } else if (!ob_is_null(types_stack[QUERY_IDX].get_type())
               && !ob_is_string_type(types_stack[QUERY_IDX].get_type())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, query must be string type");
    } else {
      types_stack[MODEL_IDX].set_calc_type(ObVarcharType);
      types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      types_stack[QUERY_IDX].set_calc_type(ObVarcharType);
      types_stack[QUERY_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    }
    ObObjType in_type = types_stack[DOCUMENTS_IDX].get_type();
    if (OB_FAIL(ret)) {
    } else if (ob_is_null(in_type)) {
    } else if (OB_FAIL(ObJsonExprHelper::is_valid_for_json(types_stack, DOCUMENTS_IDX, N_AI_RERANK))) {
    } else if (ob_is_string_type(in_type) && types_stack[DOCUMENTS_IDX].get_collation_type() != CS_TYPE_BINARY) {
      if (types_stack[DOCUMENTS_IDX].get_charset_type() != CHARSET_UTF8MB4) {
        types_stack[DOCUMENTS_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      }
    }
    if (OB_FAIL(ret)) {
    } else if (param_num == 4) {
      ObObjType doc_key_type = types_stack[DOC_KEY_IDX].get_type();
      if (doc_key_type == ObNullType) {
      } else if (ob_is_string_type(doc_key_type)) {
        types_stack[DOC_KEY_IDX].set_calc_type(ObVarcharType);
        types_stack[DOC_KEY_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      } else {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, doc key must be string type");
      }
    }

    if (OB_SUCC(ret)) {
      type.set_json();
      type.set_length((ObAccuracy::DDL_DEFAULT_ACCURACY[ObJsonType]).get_length());
    }
  }
  return ret;
}

int ObExprAIRerank::eval_ai_rerank(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res)
{
  INIT_SUCC(ret);
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_query = nullptr;
  ObDatum *arg_documents = nullptr;
  ObDatum *arg_doc_key = nullptr;
  if (OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_query, arg_documents, arg_doc_key))) {
  } else if (arg_model_id->is_null() || arg_query->is_null() || arg_documents->is_null()) {
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);

    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_RERANK));
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo resolved_endpoint;
    const share::ObAiModelEndpointInfo *endpoint_info = &resolved_endpoint;
    query::ObIAiEndpointResolver *endpoint_resolver =
        ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
    ObString model_id = arg_model_id->get_string();
    ObString query = arg_query->get_string();
    const bool has_doc_key = (OB_NOT_NULL(arg_doc_key) && !arg_doc_key->is_null());
    ObArray<ObString> header_array;
    ObJsonArray *document_array = nullptr;
    ObJsonArray *result_array = nullptr;
    ObIJsonBase *j_base = nullptr;
    ObIJsonBase *j_tree_base = nullptr;
    int64_t batch_size = 20; // max batch size for rerank
    bool is_null_result = false;
    if (model_id.empty() || query.empty()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, model id or query is empty");
    } else if (OB_FAIL(ObJsonExprHelper::get_json_doc(expr, ctx, temp_allocator, DOCUMENTS_IDX,
          j_base, is_null_result, true /*need_to_tree*/))) {
    } else if (is_null_result) {
      res.set_null();
    } else if (OB_ISNULL(j_base)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (FALSE_IT(j_tree_base = j_base)) {
    } else if (j_base->is_bin()
               && OB_FAIL(ObJsonBaseFactory::transform(&temp_allocator, j_base,
                                                       ObJsonInType::JSON_TREE, j_tree_base))) {
    } else if (OB_ISNULL(j_tree_base) || j_tree_base->json_type() != ObJsonNodeType::J_ARRAY) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array is not array");
    } else if (OB_ISNULL(document_array = static_cast<ObJsonArray *>(j_tree_base))) {
      ret = OB_ERR_UNEXPECTED;
    } else if (document_array->element_count() == 0) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array is empty");
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < document_array->element_count(); i++) {
        ObJsonNode *doc_node = document_array->get_value(i);
        if (OB_ISNULL(doc_node)) {
          ret = OB_ERR_UNEXPECTED;
        } else if (has_doc_key && doc_node->json_type() != ObJsonNodeType::J_OBJECT) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array element is not object");
        } else if (!has_doc_key && doc_node->json_type() != ObJsonNodeType::J_STRING) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array element is not string");
        }
      }
    }

    if (OB_FAIL(ret) || res.is_null()) {
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, model_id, info))) {
    } else if (OB_ISNULL(endpoint_resolver)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                   model_id, temp_allocator, resolved_endpoint))) {
    }

    if (OB_FAIL(ret) || res.is_null()) {
    } else if (has_doc_key) {
      ObString doc_key = arg_doc_key->get_string();
      if (OB_FAIL(eval_ai_rerank_with_doc_key(expr, ctx, temp_allocator, model_id,
                  query, document_array, doc_key, *info, *endpoint_info, res))) {
      }
    } else {
      if (OB_ISNULL(info)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(ObAIFuncUtils::check_info_type_rerank(info))) {
      } else if (OB_FAIL(ObAIFuncUtils::get_header(temp_allocator, *info, *endpoint_info, header_array))) {
      } else if (OB_FAIL(rerank_documents(temp_allocator, *info, *endpoint_info, header_array, query, document_array, result_array))) {
      } else {
        ObString raw_str;
        if (OB_FAIL(result_array->get_raw_binary(raw_str, &temp_allocator))) {
        } else if (OB_FAIL(ObJsonExprHelper::pack_json_str_res(expr, ctx, res, raw_str))) {
        }
      }
    }
  }
  return ret;
}

int ObExprAIRerank::rerank_documents(common::ObIAllocator &allocator,
                                     const common::ObAIFuncExprInfo &info,
                                     const share::ObAiModelEndpointInfo &endpoint_info,
                                     common::ObArray<common::ObString> &header_array,
                                     common::ObString &query,
                                     common::ObJsonArray *doc_text_array,
                                     common::ObJsonArray *&result_array)
{
  INIT_SUCC(ret);
  const int64_t batch_size = 20;
  ObString score_key(SCORE_KEY);
  int64_t end_idx = 0;
  ObJsonArray *merged_array = nullptr;
  if (OB_ISNULL(doc_text_array)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, merged_array))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < doc_text_array->element_count(); i += batch_size) {
      end_idx = i + batch_size;
      if (end_idx > doc_text_array->element_count()) {
        end_idx = doc_text_array->element_count();
      }
      const int64_t cur_batch_cnt = end_idx - i;
      ObJsonArray *batch_document_array = nullptr;
      ObJsonArray *batch_result_array = nullptr;
      ObJsonArray *sorted_batch_array = nullptr;
      ObJsonArray *compact_array = nullptr;
      if (OB_FAIL(construct_batch_document_array(allocator, doc_text_array, i, end_idx, batch_document_array))) {
      } else if (OB_FAIL(inner_eval_ai_rerank(allocator, info, endpoint_info, header_array, query, batch_document_array, batch_result_array))) {
      } else if (OB_FAIL(sort_rerank_result_array(allocator, batch_result_array, cur_batch_cnt, sorted_batch_array))) {
      } else if (OB_FAIL(batch_result_add_base(allocator, sorted_batch_array, i))) {
      } else if (OB_FAIL(compact_json_array_by_key(allocator, merged_array, sorted_batch_array, score_key, compact_array))) {
      } else {
        merged_array = compact_array;
      }
    }
  }
  if (OB_SUCC(ret)) {
    result_array = merged_array;
  }
  return ret;
}

int ObExprAIRerank::eval_ai_rerank_with_doc_key(const ObExpr &expr, ObEvalCtx &ctx, ObIAllocator &allocator,
                                                ObString& model_id, ObString& query, ObJsonArray *document_array,
                                                ObString& doc_key, const ObAIFuncExprInfo &info,
                                                const share::ObAiModelEndpointInfo &endpoint_info, ObDatum &res)
{
  UNUSED(model_id);
  INIT_SUCC(ret);
  ObJsonArray *doc_array = nullptr;
  ObJsonArray *result_array = nullptr;
  ObJsonArray *sorted_document_array = nullptr;
  ObArray<ObString> header_array;
  if (OB_FAIL(ObAIFuncUtils::check_info_type_rerank(&info))) {
  } else if (OB_FAIL(ObAIFuncUtils::get_header(allocator, info, endpoint_info, header_array))) {
  } else if (OB_FAIL(get_doc_array_from_documents_array_with_key(allocator, document_array, doc_key, doc_array))) {
  } else if (OB_FAIL(rerank_documents(allocator, info, endpoint_info, header_array, query, doc_array, result_array))) {
  } else if (OB_FAIL(sort_document_array_by_model_result(allocator, document_array, result_array, sorted_document_array))) {
  } else {
    ObString raw_str;
    if (OB_FAIL(sorted_document_array->get_raw_binary(raw_str, &allocator))) {
    } else if (OB_FAIL(ObJsonExprHelper::pack_json_str_res(expr, ctx, res, raw_str))) {
    }
  }
  return ret;
}

int ObExprAIRerank::get_doc_array_from_documents_array_with_key(ObIAllocator &allocator, ObJsonArray *document_object_array, ObString &doc_key, ObJsonArray *&doc_array)
{
  INIT_SUCC(ret);
  if (OB_ISNULL(document_object_array)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (doc_key.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObJsonArray *doc_array_obj = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, doc_array_obj))) {
    } else {
      int64_t count = document_object_array->element_count();
      ObJsonNode *doc_node = nullptr;
      ObJsonNode *doc_value_node = nullptr;
      ObJsonObject *doc_obj = nullptr;
      for (int64_t i = 0; OB_SUCC(ret) && i < count; i++) {
        doc_node = document_object_array->get_value(i);
        if (OB_ISNULL(doc_node)) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR (OB_INVALID_ARGUMENT, "ai_rerank, document_array element is null");
        } else if (doc_node->json_type() != ObJsonNodeType::J_OBJECT) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR (OB_INVALID_ARGUMENT, "ai_rerank, document_array element is not object");
        } else {
          doc_obj = static_cast<ObJsonObject *>(doc_node);
          doc_value_node = doc_obj->get_value(doc_key);
          if (OB_ISNULL(doc_value_node)) {
            ret = OB_INVALID_ARGUMENT;
            LOG_USER_ERROR (OB_INVALID_ARGUMENT, "ai_rerank, the value for given doc key is null");
          } else if (doc_value_node->json_type() != ObJsonNodeType::J_STRING) {
            ret = OB_INVALID_ARGUMENT;
            LOG_USER_ERROR (OB_INVALID_ARGUMENT, "ai_rerank, the value for given doc key is not string");
          } else if (OB_FAIL(doc_array_obj->append(doc_value_node))) {
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      doc_array = doc_array_obj;
    }
  }
  return ret;
}

int ObExprAIRerank::sort_document_array_by_model_result(ObIAllocator &allocator, ObJsonArray *document_array, ObJsonArray *model_result_array, ObJsonArray *&sorted_document_array)
{
  INIT_SUCC(ret);
  ObJsonArray *sorted_result_array = nullptr;
  ObJsonArray *sorted_document_array_obj = nullptr;
  const int64_t doc_cnt = OB_ISNULL(document_array) ? 0 : document_array->element_count();
  if (OB_FAIL(sort_rerank_result_array(allocator, model_result_array, doc_cnt, sorted_result_array))) {
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, sorted_document_array_obj))) {
  } else {
    int64_t count = sorted_result_array->element_count();
    for (int64_t i = 0; OB_SUCC(ret) && i < count; i++) {
      ObJsonNode *model_result_node = sorted_result_array->get_value(i);
      int64_t index = -1;
      double score = 0.0;
      ObJsonNode *score_value = nullptr;
      if (OB_FAIL(extract_rerank_index_and_score(model_result_node, doc_cnt, index, score, score_value))) {
      } else {
        ObJsonObject *origin_obj = static_cast<ObJsonObject *>(document_array->get_value(index));
        ObJsonObject *new_obj = nullptr;
        ObJsonNode *cloned_score = nullptr;
        if (OB_ISNULL(origin_obj)) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_ISNULL(new_obj = static_cast<ObJsonObject *>(origin_obj->clone(&allocator, true)))) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else if (OB_ISNULL(cloned_score = score_value->clone(&allocator, true))) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else if (OB_FAIL(new_obj->add(MODEL_SCORE_KEY, cloned_score, true /*with_unique_key*/, false /*is_lazy_sort*/, true /*need_overwrite*/))) {
        } else if (OB_FAIL(sorted_document_array_obj->append(new_obj))) {
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    sorted_document_array = sorted_document_array_obj;
  }
  return ret;
}

int ObExprAIRerank::construct_batch_document_array(ObIAllocator &allocator, ObJsonArray *document_array, int64_t start_idx, int64_t end_idx, ObJsonArray *&batch_document_array)
{
  INIT_SUCC(ret);
  ObJsonArray *result_array = nullptr;
  if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, result_array))) {
  } else {
    for (int64_t i = start_idx; i < end_idx; i++) {
      if (OB_FAIL(result_array->append(document_array->get_value(i)))) {
      }
    }
  }
  if (OB_SUCC(ret)) {
    batch_document_array = result_array;
  }
  return ret;
}

int ObExprAIRerank::inner_eval_ai_rerank(ObIAllocator &allocator, 
                                          const ObAIFuncExprInfo &info, 
                                          const share::ObAiModelEndpointInfo &endpoint_info,
                                          ObArray<ObString> &header_array, 
                                          ObString &query, 
                                          ObJsonArray *document_array, 
                                          ObJsonArray *&result_array)
{
  INIT_SUCC(ret);
  ObJsonArray *res = nullptr;
  ObJsonObject *config_json = nullptr;
  ObJsonObject *body = nullptr;
  ObJsonObject *http_response = nullptr;
  ObIJsonBase *response = nullptr;
  ObAIFuncClient ai_client;
  if (OB_FAIL(ObAIFuncUtils::get_rerank_body(allocator, info, endpoint_info, query, document_array, config_json, body))) {
  } else if (OB_FAIL(ai_client.send_post(allocator, endpoint_info.get_url(), header_array, body, http_response))) {
  } else if (OB_FAIL(ObAIFuncUtils::parse_rerank_output(allocator, endpoint_info, http_response, response))) {
  } else if (OB_ISNULL(response) || response->json_type() != ObJsonNodeType::J_ARRAY) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(res = static_cast<ObJsonArray *>(response))) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    result_array = res;
  }
  return ret;
}

int ObExprAIRerank::batch_result_add_base(ObIAllocator &allocator, ObJsonArray *array, int64_t start_idx)
{
  INIT_SUCC(ret);
  int64_t count = array->element_count();
  ObString index_key(INDEX_KEY);
  for (int64_t i = 0; OB_SUCC(ret) && i < count; i++) {
    ObJsonNode *node = array->get_value(i);
    if (OB_ISNULL(node) || node->json_type() != ObJsonNodeType::J_OBJECT) {
      ret = OB_INVALID_DATA;
    } else {
      ObJsonObject *obj = static_cast<ObJsonObject *>(node);
      ObJsonNode *idx_node = obj->get_value(index_key);
      if (OB_ISNULL(idx_node)) {
        ret = OB_INVALID_DATA;
      } else if (idx_node->json_type() == ObJsonNodeType::J_INT) {
        ObJsonInt *int_obj = static_cast<ObJsonInt *>(idx_node);
        int_obj->set_value(int_obj->value() + start_idx);
      } else if (idx_node->json_type() == ObJsonNodeType::J_UINT) {
        ObJsonUint *uint_obj = static_cast<ObJsonUint *>(idx_node);
        uint_obj->set_value(uint_obj->value() + static_cast<uint64_t>(start_idx));
      } else {
        ret = OB_INVALID_DATA;
      }
    }
  }
  return ret;
}

int ObExprAIRerank::compact_json_array_by_key(ObIAllocator &allocator, ObJsonArray *array1, ObJsonArray *array2, ObString &score_key, ObJsonArray *&compact_array)
{
  UNUSED(score_key);
  INIT_SUCC(ret);
  ObJsonArray *result_array = nullptr;
  if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, result_array))) {
  } else {
    int64_t count1 = array1->element_count();
    int64_t count2 = array2->element_count();
    int64_t i = 0;
    int64_t j = 0;
    while (OB_SUCC(ret) && i < count1 && j < count2) {
      ObJsonNode *node1 = array1->get_value(i);
      ObJsonNode *node2 = array2->get_value(j);
      int64_t idx1 = -1;
      int64_t idx2 = -1;
      double score1 = 0.0;
      double score2 = 0.0;
      ObJsonNode *sc_node1 = nullptr;
      ObJsonNode *sc_node2 = nullptr;
      if (OB_FAIL(extract_rerank_index_and_score(node1, 0, idx1, score1, sc_node1))) {
      } else if (OB_FAIL(extract_rerank_index_and_score(node2, 0, idx2, score2, sc_node2))) {
      } else if (score1 > score2 || (score1 == score2 && idx1 <= idx2)) {
        if (OB_FAIL(result_array->append(node1))) {
        }
        i++;
      } else {
        if (OB_FAIL(result_array->append(node2))) {
        }
        j++;
      }
    }
    while (OB_SUCC(ret) && i < count1) {
      if (OB_FAIL(result_array->append(array1->get_value(i)))) {
      }
      i++;
    }
    while (OB_SUCC(ret) && j < count2) {
      if (OB_FAIL(result_array->append(array2->get_value(j)))) {
      }
      j++;
    }
  }
  if (OB_SUCC(ret)) {
    compact_array = result_array;
  }
  return ret;
}

int ObExprAIRerank::construct_config_json(ObIAllocator &allocator, int64_t top_n, int64_t return_doc, ObJsonObject *&config_json)
{
  INIT_SUCC(ret);
  ObJsonObject *config_obj = nullptr;
  ObJsonInt *top_n_obj = nullptr;
  ObJsonBoolean *return_doc_obj = nullptr;
  bool return_doc_value = true;
  if (return_doc <= 0) {
    return_doc_value = false;
  }
  if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, config_obj))) {
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_boolean(allocator, return_doc_value, return_doc_obj))) {
  } else if (OB_FAIL(config_obj->add("return_documents", return_doc_obj))) {
  } else if (top_n > 0) {
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, top_n, top_n_obj))) {
    } else if (OB_FAIL(config_obj->add("top_n", top_n_obj))) {
    }
  }
  if (OB_SUCC(ret)) {
    config_json = config_obj;
  }
  return ret;
}

int ObExprAIRerank::cg_expr(ObExprCGCtx &expr_cg_ctx, 
                        const ObRawExpr &raw_expr,
                        ObExpr &rt_expr) const 
{
  int ret = OB_SUCCESS;
  // TODO: support schema version match in plan cache for ai func
  // const ObRawExpr *model_key = raw_expr.get_param_expr(0);
  // if (OB_NOT_NULL(model_key)
  //     && (model_key->is_static_scalar_const_expr() || model_key->is_const_expr())
  //     && model_key->get_expr_type() != T_OP_GET_USER_VAR &&
  //     OB_NOT_NULL(expr_cg_ctx.schema_guard_)) {
  //   ObIAllocator *allocator = expr_cg_ctx.allocator_;
  //   ObExecContext *exec_ctx = expr_cg_ctx.session_->get_cur_exec_ctx();
  //   bool got_data = false;
  //   ObObj const_data;
  //   ObAIFuncExprInfo *info = nullptr;
  //   if (OB_ISNULL(allocator)) {
  //     ret = OB_ERR_UNEXPECTED;
  //     LOG_WARN("allocator is null", K(ret));
  //   } else if (OB_FAIL(ObSQLUtils::calc_const_or_calculable_expr(exec_ctx,
  //                                                         model_key,
  //                                                         const_data,
  //                                                         got_data,
  //                                                         *allocator))) {
  //     LOG_WARN("failed to calc offset expr", K(ret));
  //   } else if (!got_data || const_data.is_null()) {
  //   } else {
  //     ObAIFuncExprInfo *info = OB_NEWx(ObAIFuncExprInfo, allocator, *allocator, T_FUN_SYS_AI_RERANK);
  //     if (OB_ISNULL(info)) {
  //       ret = OB_ALLOCATE_MEMORY_FAILED;
  //       LOG_WARN("failed to allocate memory for ai rerank info", K(ret));
  //     } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //       LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //     } else {
  //       rt_expr.extra_info_ = info;
  //     }
  //   }
  // }
  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIRerank::eval_ai_rerank;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
