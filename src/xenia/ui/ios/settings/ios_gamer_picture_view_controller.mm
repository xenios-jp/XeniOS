/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/settings/ios_gamer_picture_view_controller.h"

#import "xenia/ui/ios/shared/ios_theme.h"

@implementation XeniaGamerPicture
- (void)dealloc {
  [_key release];
  [_image release];
  [super dealloc];
}
@end

namespace {
NSString* const kCellID = @"picture";
constexpr CGFloat kTileSize = 76.0;
}  // namespace

@implementation XeniaGamerPictureViewController {
  NSArray<XeniaGamerPicture*>* _pictures;
  NSString* _currentKey;
  BOOL (^_choose)(NSString*);
}

- (instancetype)initWithPictures:(NSArray<XeniaGamerPicture*>*)pictures
                      currentKey:(NSString*)current_key
                          choose:(BOOL (^)(NSString*))choose {
  UICollectionViewFlowLayout* layout = [[[UICollectionViewFlowLayout alloc] init] autorelease];
  layout.itemSize = CGSizeMake(kTileSize, kTileSize);
  layout.minimumInteritemSpacing = 14;
  layout.minimumLineSpacing = 14;
  layout.sectionInset = UIEdgeInsetsMake(20, 20, 20, 20);
  if ((self = [super initWithCollectionViewLayout:layout])) {
    _pictures = [pictures copy];
    _currentKey = [current_key copy];
    _choose = [choose copy];
    self.title = NSLocalizedString(@"Gamer Picture", @"Gamer picture chooser: title");
  }
  return self;
}

- (void)dealloc {
  [_pictures release];
  [_currentKey release];
  [_choose release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.navigationItem.largeTitleDisplayMode = UINavigationItemLargeTitleDisplayModeNever;
  self.collectionView.backgroundColor = [UIColor clearColor];
  [self.collectionView registerClass:[UICollectionViewCell class]
          forCellWithReuseIdentifier:kCellID];
  [self setNeedsUpdateContentUnavailableConfiguration];
}

- (void)updateContentUnavailableConfigurationUsingState:(UIContentUnavailableConfigurationState*)
                                                            state {
  if (_pictures.count) {
    self.contentUnavailableConfiguration = nil;
    return;
  }
  UIContentUnavailableConfiguration* config = [UIContentUnavailableConfiguration emptyConfiguration];
  config.image = [UIImage systemImageNamed:@"person.crop.square"];
  config.text = NSLocalizedString(@"No Gamer Pictures", @"Gamer picture chooser: empty title");
  config.secondaryText = NSLocalizedString(
      @"The gamer pictures come with the Xbox 360 system software. Install it in Settings to "
      @"choose one.",
      @"Gamer picture chooser: empty message");
  self.contentUnavailableConfiguration = config;
}

- (NSInteger)collectionView:(UICollectionView*)collection_view
     numberOfItemsInSection:(NSInteger)section {
  return _pictures.count;
}

- (UICollectionViewCell*)collectionView:(UICollectionView*)collection_view
                 cellForItemAtIndexPath:(NSIndexPath*)index_path {
  UICollectionViewCell* cell = [collection_view dequeueReusableCellWithReuseIdentifier:kCellID
                                                                          forIndexPath:index_path];
  XeniaGamerPicture* picture = _pictures[index_path.item];
  UIBackgroundConfiguration* background = [UIBackgroundConfiguration clearConfiguration];
  background.image = picture.image;
  background.imageContentMode = UIViewContentModeScaleAspectFill;
  background.cornerRadius = kTileSize * 0.2;
  const BOOL current = [picture.key isEqualToString:_currentKey];
  background.strokeColor = current ? [XeniaTheme accent] : nil;
  background.strokeWidth = current ? 3.0 : 0.0;
  background.strokeOutset = current ? 3.0 : 0.0;
  cell.backgroundConfiguration = background;
  cell.isAccessibilityElement = YES;
  cell.accessibilityLabel = [NSString
      stringWithFormat:NSLocalizedString(@"Gamer picture %ld", @"Gamer picture chooser: "
                                                               @"VoiceOver label; %ld is a number"),
                       (long)index_path.item + 1];
  cell.accessibilityTraits =
      UIAccessibilityTraitButton | (current ? UIAccessibilityTraitSelected : 0);
  return cell;
}

- (void)collectionView:(UICollectionView*)collection_view
    didSelectItemAtIndexPath:(NSIndexPath*)index_path {
  [collection_view deselectItemAtIndexPath:index_path animated:YES];
  XeniaGamerPicture* picture = _pictures[index_path.item];
  if (_choose(picture.key)) {
    [self.navigationController popViewControllerAnimated:YES];
    return;
  }
  UIAlertController* alert = [UIAlertController
      alertControllerWithTitle:NSLocalizedString(@"Couldn't Change the Picture",
                                                 @"Gamer picture chooser: error title")
                       message:NSLocalizedString(@"Sign in to this profile, then try again.",
                                                 @"Gamer picture chooser: error message")
                preferredStyle:UIAlertControllerStyleAlert];
  [alert addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"OK", @"alert button")
                                            style:UIAlertActionStyleDefault
                                          handler:nil]];
  [self presentViewController:alert animated:YES completion:nil];
}

@end
