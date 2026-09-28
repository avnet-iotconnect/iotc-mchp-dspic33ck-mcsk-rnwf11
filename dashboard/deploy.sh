#!/usr/bin/env bash
# Deploys the MCSK BLDC dashboard (see README.md) into your own AWS account:
# an IAM role, a Lambda function, and an HTTP API in front of it. Safe to
# re-run - each step checks whether its resource already exists and updates
# it instead of failing.
#
# Run this on a machine where `aws` is configured with your own credentials
# (this repo/session never holds any).
#
# No /IOTCONNECT secret is needed here: this dashboard has no server-side
# solution key at all (see lambda_function.py's header comment) - each person
# who signs in supplies their own solution key/environment/login in the
# browser. This script just stands up the compute.
#
# Optional environment variables:
#   TEMPLATE_GUID    this project's dspic33MC device-template GUID (Settings ->
#                    Device -> Templates in the /IOTCONNECT console) - narrows
#                    every signed-in user's device picker to just this
#                    template; leave unset to show every device they can see
#   AWS_REGION       default us-east-1
#   FUNCTION_NAME    default mcsk-dashboard-api
#   ROLE_NAME        default mcsk-dashboard-lambda
#   API_NAME         default mcsk-dashboard
#
# Usage:
#   ./deploy.sh

set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

TEMPLATE_GUID="${TEMPLATE_GUID:-}"
AWS_REGION="${AWS_REGION:-us-east-1}"
FUNCTION_NAME="${FUNCTION_NAME:-mcsk-dashboard-api}"
ROLE_NAME="${ROLE_NAME:-mcsk-dashboard-lambda}"
API_NAME="${API_NAME:-mcsk-dashboard}"

command -v aws >/dev/null || { echo "aws CLI not found - install/configure it first" >&2; exit 1; }
ACCOUNT_ID=$(aws sts get-caller-identity --query Account --output text)
echo "Deploying to account $ACCOUNT_ID, region $AWS_REGION"

# --- 1. IAM role -----------------------------------------------------------
echo "== IAM role: $ROLE_NAME"
TRUST='{"Version":"2012-10-17","Statement":[{"Effect":"Allow","Principal":{"Service":"lambda.amazonaws.com"},"Action":"sts:AssumeRole"}]}'
if aws iam get-role --role-name "$ROLE_NAME" >/dev/null 2>&1; then
  echo "  role already exists"
else
  aws iam create-role --role-name "$ROLE_NAME" --assume-role-policy-document "$TRUST" >/dev/null
  echo "  created role"
fi
aws iam attach-role-policy --role-name "$ROLE_NAME" \
  --policy-arn arn:aws:iam::aws:policy/service-role/AWSLambdaBasicExecutionRole >/dev/null
ROLE_ARN=$(aws iam get-role --role-name "$ROLE_NAME" --query Role.Arn --output text)

# --- 2. package --------------------------------------------------------
echo "== packaging"
TMPZIP=$(mktemp -u /tmp/mcsk-dashboard-XXXXXX.zip)
zip -j "$TMPZIP" lambda_function.py iotc_client.py dashboard.html >/dev/null
echo "  $TMPZIP ($(du -h "$TMPZIP" | cut -f1))"

# --- 3. Lambda -----------------------------------------------------------
echo "== Lambda: $FUNCTION_NAME"
ENV_JSON=$(python3 -c "
import json
env = {}
if '$TEMPLATE_GUID':
    env['TEMPLATE_GUID'] = '$TEMPLATE_GUID'
print(json.dumps({'Variables': env}))
")
if aws lambda get-function --function-name "$FUNCTION_NAME" --region "$AWS_REGION" >/dev/null 2>&1; then
  aws lambda update-function-code --function-name "$FUNCTION_NAME" --region "$AWS_REGION" \
    --zip-file "fileb://$TMPZIP" >/dev/null
  aws lambda wait function-updated --function-name "$FUNCTION_NAME" --region "$AWS_REGION"
  aws lambda update-function-configuration --function-name "$FUNCTION_NAME" --region "$AWS_REGION" \
    --environment "$ENV_JSON" >/dev/null
  echo "  updated existing function"
else
  # A role just created can take a few seconds to become assumable.
  for i in $(seq 1 10); do
    if aws lambda create-function --function-name "$FUNCTION_NAME" --region "$AWS_REGION" \
        --runtime python3.12 --handler lambda_function.lambda_handler --role "$ROLE_ARN" \
        --timeout 30 --memory-size 256 --zip-file "fileb://$TMPZIP" \
        --environment "$ENV_JSON" >/dev/null 2>/tmp/mcsk-dashboard-create.err; then
      echo "  created function"
      break
    fi
    if grep -q "cannot be assumed" /tmp/mcsk-dashboard-create.err && [ "$i" -lt 10 ]; then
      echo "  role not yet assumable, retrying ($i/10)..."; sleep 5
    else
      cat /tmp/mcsk-dashboard-create.err >&2; exit 1
    fi
  done
fi
rm -f "$TMPZIP" /tmp/mcsk-dashboard-create.err

# --- 4. HTTP API -----------------------------------------------------------
echo "== HTTP API: $API_NAME"
LAMBDA_ARN=$(aws lambda get-function --function-name "$FUNCTION_NAME" --region "$AWS_REGION" \
  --query Configuration.FunctionArn --output text)
API_ID=$(aws apigatewayv2 get-apis --region "$AWS_REGION" \
  --query "Items[?Name=='$API_NAME'].ApiId" --output text)
if [ -z "$API_ID" ] || [ "$API_ID" = "None" ]; then
  API_ID=$(aws apigatewayv2 create-api --region "$AWS_REGION" --name "$API_NAME" \
    --protocol-type HTTP --target "$LAMBDA_ARN" --query ApiId --output text)
  echo "  created API $API_ID (quick-create: integration + \$default route + auto-deploy stage)"
else
  echo "  API $API_ID already exists"
fi
STATEMENT_ID="apigw-$API_ID"
aws lambda add-permission --function-name "$FUNCTION_NAME" --region "$AWS_REGION" \
  --statement-id "$STATEMENT_ID" --action lambda:InvokeFunction \
  --principal apigateway.amazonaws.com \
  --source-arn "arn:aws:execute-api:$AWS_REGION:$ACCOUNT_ID:$API_ID/*/*" >/dev/null 2>&1 || true

URL=$(aws apigatewayv2 get-api --api-id "$API_ID" --region "$AWS_REGION" --query ApiEndpoint --output text)
echo
echo "Done. Dashboard: $URL"
echo "Anyone can open this URL and sign in with their own /IOTCONNECT solution key,"
echo "environment, email and password - nothing was baked into this deployment."
