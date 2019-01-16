/*
 * Copyright 2018 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#include <aws/core/Aws.h>
#include <aws/core/config/AWSProfileConfigLoader.h>
#include <aws/core/utils/HashingUtils.h>
#include <aws/core/utils/Outcome.h>
#include <aws/core/utils/logging/AWSLogging.h>
#include <aws/core/utils/logging/DefaultLogSystem.h>
#include <aws/lex/LexRuntimeServiceClient.h>
#include <aws_common/sdk_utils/aws_error.h>
#include <gtest/gtest.h>

#include <lex_node/lex_node.h>
#include <ros/ros.h>

using namespace Aws;

namespace Aws {
namespace Lex {

ErrorCode BuildLexNode(LexNode &lex_node, std::shared_ptr<Client::ParameterReaderInterface> params) {
  {
    // Build a lex interactor and give it to the lex node to use it.
    // Lex has an internal conversation session, therefore the lex interactor
    // should only be available for use by one point of entry.
    auto lex_interactor = std::unique_ptr<Aws::Lex::LexInteractor>(new Aws::Lex::LexInteractor());
    auto error_code = Aws::Lex::BuildLexInteractor(params, '/', *lex_interactor);
    if (error_code != ErrorCode::SUCCESS) {
      return error_code;
    }
    error_code = lex_node.Init(std::move(lex_interactor));
    return error_code;
  }
}
/**
 * Post content to lex given an audio text conversation request and respond to it.
 * Configures the call with the lex configuration and lex_runtime_client.
 *
 * @param request to populate the lex call with
 * @param response to fill with data received by lex
 * @param lex_configuration to specify bot, and response type
 * @param lex_runtime_client to call lex with
 * @return true if the call succeeded, false otherwise
 */
bool PostContent(
  lex_common_msgs::AudioTextConversationRequest & request,
  lex_common_msgs::AudioTextConversationResponse & response,
  const LexConfiguration & lex_configuration,
  std::shared_ptr<const LexRuntimeService::LexRuntimeServiceClient> lex_runtime_client);

}  // namespace Lex
}  // namespace Aws

class LexNodeSuite : public ::testing::Test
{
protected:
  LexNodeSuite() : configuration_('/')
  {
    options_.loggingOptions.logLevel = Utils::Logging::LogLevel::Trace;

    request_.content_type = "text/plain; charset=utf-8";
    request_.accept_type = "text/plain; charset=utf-8";
    request_.text_request = "make a reservation";

    configuration_.user_id = "test_user";
    configuration_.bot_name = "test_bot";
    configuration_.bot_alias = "superbot";
  }

  void SetUp() override
  {
    InitAPI(options_);
    Utils::Logging::InitializeAWSLogging(MakeShared<Utils::Logging::DefaultLogSystem>(
      "lex_node_test", Utils::Logging::LogLevel::Trace, "aws_sdk_"));
  }

  void TearDown() override
  {
    Utils::Logging::ShutdownAWSLogging();
    ShutdownAPI(options_);
  }

  SDKOptions options_;
  lex_common_msgs::AudioTextConversationRequest request_;
  Lex::LexConfiguration configuration_;
};

/**
 * Parameter reader that sets the output using provided std::mapS.
 */
class TestParameterReader : public Client::ParameterReaderInterface
{
public:
  TestParameterReader() {}

  TestParameterReader(const std::string & user_id, const std::string & bot_name,
                      const std::string & bot_alias)
  {
    int_map_ = {{"aws_client_configuration/connect_timeout_ms", 9000},
                {"aws_client_configuration/request_timeout_ms", 9000}};
    Lex::LexConfiguration configuration('/');
    string_map_ = {{configuration.user_id_key, user_id},
                   {configuration.bot_name_key, bot_name},
                   {configuration.bot_alias_key, bot_alias},
                   {"aws_client_configuration/region", "us-west-2"}};
  }

  AwsError ReadInt(const char * name, int & out) const
  {
    AwsError result = AWS_ERR_NOT_FOUND;
    if (int_map_.count(name) > 0) {
      out = int_map_.at(name);
      result = AWS_ERR_OK;
    }
    return result;
  }
  AwsError ReadBool(const char * name, bool & out) const { return AWS_ERR_NOT_FOUND; }
  AwsError ReadStdString(const char * name, std::string & out) const
  {
    AwsError result = AWS_ERR_NOT_FOUND;
    if (string_map_.count(name) > 0) {
      out = string_map_.at(name);
      result = AWS_ERR_OK;
    }
    return result;
  }
  AwsError ReadString(const char * name, String & out) const
  {
    AwsError result = AWS_ERR_NOT_FOUND;
    if (string_map_.count(name) > 0) {
      out = string_map_.at(name).c_str();
      result = AWS_ERR_OK;
    }
    return result;
  }
  AwsError ReadMap(const char * name, std::map<std::string, std::string> & out) const
  {
    return AWS_ERR_NOT_FOUND;
  }
  AwsError ReadList(const char * name, std::vector<std::string> & out) const
  {
    return AWS_ERR_NOT_FOUND;
  }
  AwsError ReadDouble(const char * name, double & out) const { return AWS_ERR_NOT_FOUND; }

  std::map<std::string, int> int_map_;
  std::map<std::string, std::string> string_map_;
};

class MockLexClient : public LexRuntimeService::LexRuntimeServiceClient
{
public:
  MockLexClient(bool succeed = false) : succeed_(succeed) {}

  // MockLexClient(LexRuntimeService::Model::PostContentOutcome outcome) : outcome_(outcome) {}

  virtual LexRuntimeService::Model::PostContentOutcome PostContent(
    const LexRuntimeService::Model::PostContentRequest & request) const override
  {
    if (succeed_) {
      LexRuntimeService::Model::PostContentResult result;

      result.SetContentType("test_content_type");

      result.SetIntentName("test_intent_name");

      constexpr unsigned char slot_string[] =
        "{\"test_slots_key1\": \"test_slots_value1\", \"test_slots_key2\": \"test_slots_value2\"}";
      Utils::ByteBuffer slot_buffer(slot_string, sizeof(slot_string));
      auto slot_stdstring = Utils::HashingUtils::Base64Encode(slot_buffer);
      result.SetSlots(slot_stdstring);

      result.SetSessionAttributes("test_session_attributes");

      result.SetMessage("test_message");

      result.SetMessageFormat(LexRuntimeService::Model::MessageFormatType::CustomPayload);

      result.SetDialogState(LexRuntimeService::Model::DialogState::Failed);

      result.SetSlotToElicit("test_active_slot");

      std::stringstream * audio_data = New<std::stringstream>("test");
      *audio_data << "blah blah blah";
      result.ReplaceBody(audio_data);

      return LexRuntimeService::Model::PostContentOutcome(std::move(result));
    } else {
      return LexRuntimeService::Model::PostContentOutcome(
        Client::AWSError<LexRuntimeService::LexRuntimeServiceErrors>());
    }
  }

private:
  bool succeed_;
};

/**
 * Tests the creation of a Lex node instance with invalid parameters
 */
TEST_F(LexNodeSuite, BuildLexNodeWithEmptyParams)
{
  auto param_reader = std::make_shared<TestParameterReader>();

  Lex::LexNode lex_node;
  auto error_code = Lex::BuildLexNode(lex_node, param_reader);
  ASSERT_EQ(INVALID_LEX_CONFIGURATION, error_code);
}

/**
 * Tests the creation of a Lex node instance with null LexInteractor
 */
TEST_F(LexNodeSuite, BuildLexNodeWithNullLexInteractor)
{
  auto param_reader = std::make_shared<TestParameterReader>();

  Lex::LexNode lex_node;
  auto error_code = lex_node.Init(std::unique_ptr<Lex::LexInteractor>());
  ASSERT_EQ(INVALID_ARGUMENT, error_code);
}

/**
 * Test the result of PostContent() when the Lex runtime client fails to PostContent()
 */
TEST_F(LexNodeSuite, LexNodePostContentFail)
{
  auto param_reader = std::make_shared<TestParameterReader>(
    configuration_.user_id, configuration_.bot_name, configuration_.bot_alias);
  Lex::LexNode lex_node;
  ErrorCode error = Lex::BuildLexNode(lex_node, param_reader);
  ASSERT_EQ(SUCCESS, error);

  auto lex_runtime_client = std::make_shared<MockLexClient>(false);

  lex_common_msgs::AudioTextConversationResponse response;
  bool success = PostContent(request_, response);
  EXPECT_FALSE(success);

  // check that the response hasn't been filled because PostContent() was supposed to have failed
  EXPECT_TRUE(response.text_response.empty());
  EXPECT_TRUE(response.audio_response.data.empty());
  EXPECT_TRUE(response.slots.empty());
  EXPECT_TRUE(response.intent_name.empty());
  EXPECT_TRUE(response.message_format_type.empty());
  EXPECT_TRUE(response.dialog_state.empty());
}

/**
 * Test the result of PostContent() when the Lex runtime client fails to PostContent()
 */
TEST_F(LexNodeSuite, LexNodePostContentSucceed)
{
  auto param_reader = std::make_shared<TestParameterReader>(
    configuration_.user_id, configuration_.bot_name, configuration_.bot_alias);
  Lex::LexNode lex_node;
  Lex::BuildLexNode(lex_node, param_reader);

  auto lex_runtime_client = std::make_shared<MockLexClient>(true);

  lex_common_msgs::AudioTextConversationResponse response;
  bool success = PostContent(request_, response, configuration_, lex_runtime_client);
  EXPECT_TRUE(success);

  // check that the response hasn't been filled because PostContent() was supposed to have failed
  EXPECT_EQ(response.text_response, "test_message");
  EXPECT_TRUE(0 == memcmp(response.audio_response.data.data(), "blah blah blah", 14));
  EXPECT_EQ(response.slots.size(), 2);
  EXPECT_EQ(response.slots[0].key, "test_slots_key1");
  EXPECT_EQ(response.slots[0].value, "test_slots_value1");
  EXPECT_EQ(response.slots[1].key, "test_slots_key2");
  EXPECT_EQ(response.slots[1].value, "test_slots_value2");
  EXPECT_EQ(response.intent_name, "test_intent_name");
  EXPECT_EQ(response.message_format_type, "CustomPayload");
  EXPECT_EQ(response.dialog_state, "Failed");
}

int main(int argc, char ** argv)
{
  testing::InitGoogleTest(&argc, argv);
  ros::init(argc, argv, "test_lex_node");
  return RUN_ALL_TESTS();
}
