/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this file
 * except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.openairinterface.org/?page_id=698
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

#ifndef OAI_NAS_STREAM_EEA1_H
#define OAI_NAS_STREAM_EEA1_H

#include "secu_defs.h"

void nas_stream_encrypt_eea1(nas_stream_cipher_t const *stream_cipher, uint8_t *out);

stream_security_context_t *stream_ciphering_init_eea1(const uint8_t *ciphering_key);
void stream_ciphering_free_eea1(stream_security_context_t *ciphering_context);

#endif
