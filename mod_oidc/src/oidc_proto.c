/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/***************************************************************************
 * Copyright (C) 2013-2014 Ping Identity Corporation
 * All rights reserved.
 *
 * The contents of this file are the property of Ping Identity Corporation.
 * For further information please contact:
 *
 *      Ping Identity Corporation
 *      1099 18th St Suite 2950
 *      Denver, CO 80202
 *      303.468.2900
 *      http://www.pingidentity.com
 *
 * DISCLAIMER OF WARRANTIES:
 *
 * THE SOFTWARE PROVIDED HEREUNDER IS PROVIDED ON AN "AS IS" BASIS, WITHOUT
 * ANY WARRANTIES OR REPRESENTATIONS EXPRESS, IMPLIED OR STATUTORY; INCLUDING,
 * WITHOUT LIMITATION, WARRANTIES OF QUALITY, PERFORMANCE, NONINFRINGEMENT,
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.  NOR ARE THERE ANY
 * WARRANTIES CREATED BY A COURSE OR DEALING, COURSE OF PERFORMANCE OR TRADE
 * USAGE.  FURTHERMORE, THERE ARE NO WARRANTIES THAT THE SOFTWARE WILL MEET
 * YOUR NEEDS OR BE FREE FROM ERRORS, OR THAT THE OPERATION OF THE SOFTWARE
 * WILL BE UNINTERRUPTED.  IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * @Author: Hans Zandbelt - hans.zandbelt@gmail.com
 */

#include <httpd.h>
#include <http_config.h>
#include <http_log.h>
#include <http_request.h>

#include "mod_oidc.h"

/*
 * send an OpenID Connect authorization request to the specified provider
 */
int oidc_proto_authorization_request(request_rec *r,
		struct oidc_provider_t *provider, const char *redirect_uri,
		const char *state, const char *original_url) {

	/* log some stuff */
	ap_log_rerror(APLOG_MARK, OIDC_DEBUG, 0, r,
			"oidc_proto_authorization_request: entering (issuer=%s, original_url=%s)",
			provider->issuer, original_url);

	/* assemble the full URL as the authorization request to the OP where we want to redirect to */
	char *destination =
			apr_psprintf(r->pool,
					"%s%sresponse_type=%s&scope=%s&client_id=%s&state=%s&redirect_uri=%s",
					provider->authorization_endpoint_url,
					(strchr(provider->authorization_endpoint_url, '?') != NULL ?
							"&" : "?"), "code",
							oidc_util_escape_string(r, provider->scope),
							oidc_util_escape_string(r, provider->client_id),
							oidc_util_escape_string(r, state),
							oidc_util_escape_string(r, redirect_uri));

	/* add the redirect location header */
	apr_table_add(r->headers_out, "Location", destination);

	/* some more logging */
	ap_log_rerror(APLOG_MARK, OIDC_DEBUG, 0, r,
			"oidc_proto_authorization_request: adding outgoing header: Location: %s",
			destination);

	/* and tell Apache to return an HTTP Redirect (302) message */
	return HTTP_MOVED_TEMPORARILY;
}

/*
 * indicate whether the incoming HTTP request is an OpenID Connect Authorization Response, syntax-wise
 */
apr_byte_t oidc_proto_is_authorization_response(request_rec *r, oidc_cfg *cfg) {

	/* see if this is a call to the configured redirect_uri and the "code" and "state" parameters are present */
	return ((oidc_util_request_matches_url(r, cfg->redirect_uri) == TRUE)
			&& oidc_util_request_has_parameter(r, "code")
			&& oidc_util_request_has_parameter(r, "state"));
}

/*
 * check whether the provided JSON payload (in the j_payload parameter) is a valid id_token for the specified "provider"
 */
static apr_byte_t oidc_proto_is_valid_idtoken(request_rec *r,
		oidc_provider_t *provider, apr_json_value_t *j_payload,
		apr_time_t *expires) {

	ap_log_rerror(APLOG_MARK, OIDC_DEBUG, 0, r,
			"oidc_proto_is_valid_idtoken: entering");

	/* get the "issuer" value from the JSON payload */
	apr_json_value_t *iss = apr_hash_get(j_payload->value.object, "iss",
			APR_HASH_KEY_STRING);
	if ((iss == NULL) || (iss->type != APR_JSON_STRING)) {
		ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
				"oidc_proto_is_valid_idtoken: response JSON object did not contain an \"iss\" string");
		return FALSE;
	}

	/* check the "issuer" value against the one configure for the provider we got this id_token from */
	if (strcmp(provider->issuer, iss->value.string.p) != 0) {
		/* no strict match, but we are going to accept if the difference is only a trailing slash */
		int n1 = strlen(provider->issuer);
		int n2 = strlen(iss->value.string.p);
		int n = ((n1 == n2 + 1) && (provider->issuer[n1 - 1] == '/')) ?
				n2 :
				(((n2 == n1 + 1) && (iss->value.string.p[n2 - 1] == '/')) ?
						n1 : 0);
		if ((n == 0)
				|| (strncmp(provider->issuer, iss->value.string.p, n) != 0)) {
			ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
					"oidc_proto_is_valid_idtoken: configured issuer (%s) does not match received \"iss\" value in id_token (%s)",
					provider->issuer, iss->value.string.p);
			return FALSE;
		}
	}

	/* get the "exp" value from the JSON payload */
	apr_json_value_t *exp = apr_hash_get(j_payload->value.object, "exp",
			APR_HASH_KEY_STRING);
	if ((exp == NULL) || (exp->type != APR_JSON_LONG)) {
		ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
				"oidc_proto_is_valid_idtoken: response JSON object did not contain an \"exp\" number");
		return FALSE;
	}

	/* check if this id_token has already expired */
	if (apr_time_sec(apr_time_now()) > exp->value.lnumber) {
		ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
				"oidc_proto_is_valid_idtoken: id_token expired");
		return FALSE;
	}

	/* return the "exp" value in the "expires" return parameter */
	*expires = apr_time_from_sec(exp->value.lnumber);

	/* get the "azp" value from the JSON payload, which may be NULL */
	apr_json_value_t *azp = apr_hash_get(j_payload->value.object, "azp",
			APR_HASH_KEY_STRING);
	if ((azp != NULL) && (azp->type != APR_JSON_STRING)) {
		ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
				"oidc_proto_is_valid_idtoken: id_token JSON payload contained an \"azp\" value, but it was not a string");
		return FALSE;
	}

	/*
	 * This Claim is only needed when the ID Token has a single audience value and that audience is different than the authorized party.
	 * It MAY be included even when the authorized party is the same as the sole audience.
	 */
	if ((azp != NULL)
			&& (strcmp(azp->value.string.p, provider->client_id) != 0)) {
		ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
				"\"azp\" claim (%s) is not equal to configured client_id (%s)",
				azp->value.string.p, provider->client_id);
		return FALSE;
	}

	/* get the "aud" value from the JSON payload */
	apr_json_value_t *aud = apr_hash_get(j_payload->value.object, "aud",
			APR_HASH_KEY_STRING);

	if (aud != NULL) {

		/* check if it is a single-value */
		if (aud->type == APR_JSON_STRING) {

			/* a single-valued audience must be equal to our client_id */
			if (strcmp(aud->value.string.p, provider->client_id) != 0) {

				ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
						"oidc_proto_is_valid_idtoken: configured client_id (%s) did not match the JSON \"aud\" entry (%s)",
						provider->client_id, aud->value.string.p);
				return FALSE;
			}

			/* check if this is a multi-valued audience */
		} else if (aud->type == APR_JSON_ARRAY) {

			if ((aud->value.array->nelts > 1) && (azp == NULL)) {
				ap_log_rerror(APLOG_MARK, OIDC_DEBUG, 0, r,
						"oidc_proto_is_valid_idtoken: \"aud\" is an array with more than 1 element, but \"azp\" claim is not present (a SHOULD in the spec...)");
			}

			/* loop over the audience values */
			int i;
			for (i = 0; i < aud->value.array->nelts; i++) {

				apr_json_value_t *elem =
						APR_ARRAY_IDX(aud->value.array, i, apr_json_value_t *);

				/* check if it is a string, warn otherwise */
				if (elem->type != APR_JSON_STRING) {
					ap_log_rerror(APLOG_MARK, APLOG_WARNING, 0, r,
							"oidc_proto_is_valid_idtoken: unhandled in-array JSON object type [%d]",
							elem->type);
					continue;
				}

				/* we're looking for a value in the list that matches our client id */
				if (strcmp(elem->value.string.p, provider->client_id) == 0) {
					break;
				}
			}

			/* check if we've found a match or not */
			if (i == aud->value.array->nelts) {

				ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
						"oidc_proto_is_valid_idtoken: configured client_id (%s) could not be found in the JSON \"aud\" array object",
						provider->client_id);
				return FALSE;
			}

		} else {

			ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
					"oidc_proto_is_valid_idtoken: response JSON \"aud\" object is not a string nor an array");
			return FALSE;
		}

	} else {

		ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
				"oidc_proto_is_valid_idtoken: response JSON object did not contain an \"aud\" element");
		return FALSE;
	}

	return TRUE;
}

/*
 * check whether the provider string is a valid id_token for the specified "provider"
 */
static apr_byte_t oidc_proto_is_valid_idtoken_payload(request_rec *r,
		oidc_provider_t *provider, const char *s_idtoken_payload,
		apr_json_value_t **result, apr_time_t *expires) {

	ap_log_rerror(APLOG_MARK, OIDC_DEBUG, 0, r,
			"oidc_proto_is_valid_idtoken_payload: entering (%s)", s_idtoken_payload);

	/* decode the string in to a JSON structure */
	if (apr_json_decode(result, s_idtoken_payload, strlen(s_idtoken_payload),
			r->pool) != APR_SUCCESS) {
		ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
				"oidc_proto_is_valid_idtoken_payload: could not decode id_token payload string in to a JSON structure");
		return FALSE;
	}

	/* a convenient helper pointer */
	apr_json_value_t *j_payload = *result;

	/* check that we've actually got a JSON object back */
	if ((j_payload == NULL) || (j_payload->type != APR_JSON_OBJECT)) {
		ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
				"oidc_proto_is_valid_idtoken_payload: payload from id_token did not contain a JSON object");
		return FALSE;
	}

	/* now check if the JSON is a valid id_token */
	return oidc_proto_is_valid_idtoken(r, provider, j_payload, expires);
}

/*
 * check whether the provided string is a valid id_token header
 */
static apr_byte_t oidc_proto_parse_idtoken_header(request_rec *r,
		const char *s_header, apr_json_value_t **result) {

	ap_log_rerror(APLOG_MARK, OIDC_DEBUG, 0, r,
			"oidc_proto_parse_idtoken_header: entering");

	/* a convenient helper pointer */
	apr_json_value_t *j_header = *result;

	/* decode the string in to a JSON structure */
	if (apr_json_decode(&j_header, s_header, strlen(s_header),
			r->pool) != APR_SUCCESS) {
		ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
				"oidc_proto_parse_idtoken_header: could not decode header from id_token successfully");
		return FALSE;
	}

	/* check that we've actually got a JSON object back */
	if ((j_header == NULL) || (j_header->type != APR_JSON_OBJECT)) {
		ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
				"oidc_proto_parse_idtoken_header: header from id_token did not contain a JSON object");
		return FALSE;
	}

	/* this is all we check for now because we use the "code flow" */
	return TRUE;
}

/*
 * check whether the provided string is a valid id_token and return its parsed contents
 */
static apr_byte_t oidc_proto_parse_idtoken(request_rec *r,
		oidc_provider_t *provider, const char *id_token, char **user,
		apr_json_value_t **j_payload, apr_time_t *expires) {

	ap_log_rerror(APLOG_MARK, OIDC_DEBUG, 0, r,
			"oidc_proto_parse_idtoken: entering");

	/* find the header */
	const char *s = id_token;
	char *p = strchr(s, '.');
	if (p == NULL) {
		ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
				"oidc_proto_parse_id_token: could not find first \".\" in id_token");
		return FALSE;
	}
	*p = '\0';

	/* parse the header (base64decode, json_decode) and validate it */
	char *header = NULL;
	oidc_base64url_decode(r, &header, s, 1);
	apr_json_value_t *j_header = NULL;
	oidc_proto_parse_idtoken_header(r, header, &j_header);

	/* find the payload */
	s = ++p;
	p = strchr(s, '.');
	if (p == NULL) {
		ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
				"oidc_proto_parse_id_token: could not find second \".\" in id_token");
		return FALSE;
	}
	*p = '\0';

	/* parse the payload */
	char *s_payload = NULL;
	oidc_base64url_decode(r, &s_payload, s, 1);

	/* this is where the meat is */
	if (oidc_proto_is_valid_idtoken_payload(r, provider, s_payload, j_payload,
			expires) == FALSE)
		return FALSE;

	/* extract and return the user name claim ("sub") */
	apr_json_value_t *username = apr_hash_get((*j_payload)->value.object, "sub",
			APR_HASH_KEY_STRING);
	if ((username == NULL) || (username->type != APR_JSON_STRING)) {
		ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
				"oidc_proto_parse_id_token: response JSON object did not contain a \"sub\" string");
		return FALSE;
	}
	*user = apr_pstrdup(r->pool, username->value.string.p);

	/* find the signature, no further checking yet */
	//s = ++p;
	//char *signature = apr_pstrdup(r->pool, s);

	/* log our results */
	ap_log_rerror(APLOG_MARK, OIDC_DEBUG, 0, r,
			"oidc_proto_parse_idtoken: valid id_token for user \"%s\" (expires in %" APR_TIME_T_FMT " seconds)",
			username->value.string.p, *expires - apr_time_sec(apr_time_now()));

	/* since we've made it so far, we may as well say it is a valid id_token */
	return TRUE;
}

/*
 * resolves the code received from the OP in to an access_token and id_token and returns the parsed contents
 */
apr_byte_t oidc_proto_resolve_code(request_rec *r, oidc_cfg *cfg,
		oidc_provider_t *provider, char *code, char **user,
		apr_json_value_t **j_idtoken_payload, char **s_id_token,
		char **s_access_token, apr_time_t *expires) {

	ap_log_rerror(APLOG_MARK, OIDC_DEBUG, 0, r,
			"oidc_proto_resolve_code: entering");
	const char *response = NULL;

	/* assemble the parameters for a call to the token endpoint */
	apr_table_t *params = apr_table_make(r->pool, 5);
	apr_table_addn(params, "grant_type", "authorization_code");
	apr_table_addn(params, "code", code);
	apr_table_addn(params, "redirect_uri", cfg->redirect_uri);

	/* see if we need to do basic auth or auth-through-post-params (both applied through the HTTP POST method though) */
	const char *basic_auth = NULL;
	if ((apr_strnatcmp(provider->token_endpoint_auth, "client_secret_basic"))
			== 0) {
		basic_auth = apr_psprintf(r->pool, "%s:%s", provider->client_id,
				provider->client_secret);
	} else {
		apr_table_addn(params, "client_id", provider->client_id);
		apr_table_addn(params, "client_secret", provider->client_secret);
	}

	/* resolve the code against the token endpoint */
	if (oidc_util_http_call(r, provider->token_endpoint_url,
			OIDC_HTTP_POST_FORM, params, basic_auth, NULL,
			provider->ssl_validate_server, &response,
			cfg->http_timeout_long) == FALSE) {
		ap_log_rerror(APLOG_MARK, OIDC_DEBUG, 0, r,
				"oidc_proto_resolve_code: could not succesfully resolve the \"code\" (%s) against the tokent endpont (%s)",
				code, provider->token_endpoint_url);
		return FALSE;
	}

	/* check for errors, the response itself will have been logged already */
	apr_json_value_t *result = NULL;
	if (oidc_util_decode_json_and_check_error(r, response, &result) == FALSE)
		return FALSE;

	/* get the access_token from the parsed response */
	apr_json_value_t *access_token = apr_hash_get(result->value.object,
			"access_token", APR_HASH_KEY_STRING);
	if ((access_token == NULL) || (access_token->type != APR_JSON_STRING)) {
		ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
				"oidc_proto_resolve_code: response JSON object did not contain an access_token string");
		return FALSE;
	}

	/* log and set the obtained acces_token */
	ap_log_rerror(APLOG_MARK, OIDC_DEBUG, 0, r,
			"oidc_proto_resolve_code: returned access_token: %s",
			access_token->value.string.p);
	*s_access_token = apr_pstrdup(r->pool, access_token->value.string.p);

	/* the provider must the token type */
	apr_json_value_t *token_type = apr_hash_get(result->value.object,
			"token_type", APR_HASH_KEY_STRING);
	if ((token_type == NULL) || (token_type->type != APR_JSON_STRING)) {
		ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
				"oidc_proto_resolve_code: response JSON object did not contain a token_type string");
		return FALSE;
	}

	/* we got the type, we only support bearer/Bearer, check that */
	if ((apr_strnatcasecmp(token_type->value.string.p, "Bearer") != 0)
			&& (provider->userinfo_endpoint_url != NULL)) {
		ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
				"oidc_proto_resolve_code: token_type is \"%s\" and UserInfo endpoint is set: can only deal with Bearer authentication against the UserInfo endpoint!",
				token_type->value.string.p);
		return FALSE;
	}

	/* get the id_token from the response */
	apr_json_value_t *id_token = apr_hash_get(result->value.object, "id_token",
			APR_HASH_KEY_STRING);
	if ((id_token == NULL) || (id_token->type != APR_JSON_STRING)) {
		ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
				"oidc_proto_resolve_code: response JSON object did not contain an id_token string");
		return FALSE;
	}

	/* log and set the obtained id_token */
	ap_log_rerror(APLOG_MARK, OIDC_DEBUG, 0, r,
			"oidc_proto_resolve_code: returned id_token: %s",
			id_token->value.string.p);
	*s_id_token = apr_pstrdup(r->pool, id_token->value.string.p);

	/* parse and validate the obtained id_token and return success/failure of that */
	return oidc_proto_parse_idtoken(r, provider, id_token->value.string.p, user,
			j_idtoken_payload, expires);
}

/*
 * get claims from the OP UserInfo endpoint using the provided access_token
 */
apr_byte_t oidc_proto_resolve_userinfo(request_rec *r, oidc_cfg *cfg,
		oidc_provider_t *provider, const char *access_token,
		const char **response, apr_json_value_t **claims) {

	ap_log_rerror(APLOG_MARK, OIDC_DEBUG, 0, r,
			"oidc_resolve_userinfo: entering, endpoint=%s, access_token=%s",
			provider->userinfo_endpoint_url, access_token);

	/* only do this if an actual endpoint was set */
	if (provider->userinfo_endpoint_url == NULL)
		return FALSE;

	/* get the JSON response */
	if (oidc_util_http_call(r, provider->userinfo_endpoint_url, OIDC_HTTP_GET,
			NULL, NULL, access_token, provider->ssl_validate_server, response,
			cfg->http_timeout_long) == FALSE)
		return FALSE;

	/* decode and check for an "error" response */
	return oidc_util_decode_json_and_check_error(r, *response, claims);
}

/*
 * based on an account name, perform OpenID Connect Provider Issuer Discovery to find out the issuer and obtain and store its metadata
 */
apr_byte_t oidc_proto_account_based_discovery(request_rec *r, oidc_cfg *cfg,
		const char *acct, char **issuer) {

	// TODO: maybe show intermediate/progress screen "discovering..."

	ap_log_rerror(APLOG_MARK, OIDC_DEBUG, 0, r,
			"oidc_proto_account_based_discovery: entering, acct=%s", acct);

	const char *resource = apr_psprintf(r->pool, "acct:%s", acct);
	const char *domain = strrchr(acct, '@');
	if (domain == NULL) {
		ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
				"oidc_proto_account_based_discovery: invalid account name");
		return FALSE;
	}
	domain++;
	const char *url = apr_psprintf(r->pool, "https://%s/.well-known/webfinger",
			domain);

	apr_table_t *params = apr_table_make(r->pool, 1);
	apr_table_addn(params, "resource", resource);
	apr_table_addn(params, "rel", "http://openid.net/specs/connect/1.0/issuer");

	const char *response = NULL;
	if (oidc_util_http_call(r, url, OIDC_HTTP_GET, params, NULL, NULL,
			cfg->provider.ssl_validate_server, &response,
			cfg->http_timeout_short) == FALSE) {
		/* errors will have been logged by now */
		return FALSE;
	}

	/* decode and see if it is not an error response somehow */
	apr_json_value_t *j_response = NULL;
	if (oidc_util_decode_json_and_check_error(r, response, &j_response) == FALSE)
		return FALSE;

	/* get the links parameter */
	apr_json_value_t *j_links = apr_hash_get(j_response->value.object, "links",
			APR_HASH_KEY_STRING);
	if ((j_links == NULL) || (j_links->type != APR_JSON_ARRAY)) {
		ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
				"oidc_proto_account_based_discovery: response JSON object did not contain a \"links\" array");
		return FALSE;
	}

	/* get the one-and-only object in the "links" array */
	apr_json_value_t *j_object =
			((apr_json_value_t**) j_links->value.array->elts)[0];
	if ((j_object == NULL) || (j_object->type != APR_JSON_OBJECT)) {
		ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
				"oidc_proto_account_based_discovery: response JSON object did not contain a JSON object as the first element in the \"links\" array");
		return FALSE;
	}

	/* get the href from that object, which is the issuer value */
	apr_json_value_t *j_href = apr_hash_get(j_object->value.object, "href",
			APR_HASH_KEY_STRING);
	if ((j_href == NULL) || (j_href->type != APR_JSON_STRING)) {
		ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
				"oidc_proto_account_based_discovery: response JSON object did not contain a \"href\" element in the first \"links\" array object");
		return FALSE;
	}

	*issuer = (char *) j_href->value.string.p;

	ap_log_rerror(APLOG_MARK, OIDC_DEBUG, 0, r,
			"oidc_proto_account_based_discovery: returning issuer \"%s\" for account \"%s\" after doing succesful webfinger-based discovery",
			*issuer, acct);

	return TRUE;
}
